/*
 * Copyright (c) 2009, OmniTI Computer Consulting, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name OmniTI Computer Consulting, Inc. nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Theo Schlossnagle
 *
 */

#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "postgres.h"
#include "funcapi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "executor/spi.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "librabbitmq/amqp.h"
#include "librabbitmq/amqp_framing.h"

#define set_bytes_from_text(var,col) do { \
  if(!PG_ARGISNULL(col)) { \
    text *txt = PG_GETARG_TEXT_PP(col); \
    var.bytes = VARDATA_ANY(txt); \
    var.len = VARSIZE_ANY_EXHDR(txt); \
  } \
} while(0)

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
void _PG_init(void);
Datum pg_amqp_exchange_declare(PG_FUNCTION_ARGS);
Datum pg_amqp_publish(PG_FUNCTION_ARGS);
Datum pg_amqp_autonomous_publish(PG_FUNCTION_ARGS);
Datum pg_amqp_disconnect(PG_FUNCTION_ARGS);

struct brokerstate {
  int broker_id;
  amqp_connection_state_t conn;
  int sockfd;
  int uncommitted;
  int inerror;
  int idx;
  struct brokerstate *next;
};

static struct brokerstate *HEAD_BS = NULL;

static void
local_amqp_disconnect_bs(struct brokerstate *bs) {
  if(bs && bs->conn) {
    int errorstate = bs->inerror;
    amqp_connection_close(bs->conn, AMQP_REPLY_SUCCESS);
    if(bs->sockfd >= 0) close(bs->sockfd);
    amqp_destroy_connection(bs->conn);
    memset(bs, 0, sizeof(*bs));
    bs->inerror = errorstate;
  }
}
static void amqp_local_phase2(XactEvent event, void *arg) {
  amqp_rpc_reply_t *reply;
  struct brokerstate *bs;
  switch(event) {
    case XACT_EVENT_COMMIT:
      for(bs = HEAD_BS; bs; bs = bs->next) {
        if(bs->inerror) local_amqp_disconnect_bs(bs);
        bs->inerror = 0;
        if(!bs->uncommitted) continue;
        if(bs->conn) amqp_tx_commit(bs->conn, 2, AMQP_EMPTY_TABLE);
        reply = amqp_get_rpc_reply();
        if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
          elog(WARNING, "amqp could not commit tx mode on broker %d, reply_type=%d, library_errno=%d", bs->broker_id, reply->reply_type, reply->library_errno);
          local_amqp_disconnect_bs(bs);
        }
        bs->uncommitted = 0;
      }
      break;
    case XACT_EVENT_ABORT:
      for(bs = HEAD_BS; bs; bs = bs->next) {
        if(bs->inerror) local_amqp_disconnect_bs(bs);
        bs->inerror = 0;
        if(!bs->uncommitted) continue;
        if(bs->conn) amqp_tx_rollback(bs->conn, 2, AMQP_EMPTY_TABLE);
        reply = amqp_get_rpc_reply();
        if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
          elog(WARNING, "amqp could not rollback tx mode on broker %d, reply_type=%d, library_errno=%d", bs->broker_id, reply->reply_type, reply->library_errno);
          local_amqp_disconnect_bs(bs);
        }
        bs->uncommitted = 0;
      }
      break;
    case XACT_EVENT_PREPARE:
      /* nothin' */
      return;
      break;
  }
}

void _PG_init() {
  RegisterXactCallback(amqp_local_phase2, NULL);
}

static struct brokerstate *
local_amqp_get_a_bs(int broker_id) {
  struct brokerstate *bs;
  for(bs = HEAD_BS; bs; bs = bs->next) {
    if(bs->broker_id == broker_id) return bs;
  }
  bs = MemoryContextAllocZero(TopMemoryContext, sizeof(*bs));
  bs->broker_id = broker_id;
  bs->next = HEAD_BS;
  HEAD_BS = bs;
  return bs;
}
static struct brokerstate *
local_amqp_get_bs(int broker_id) {
  char sql[1024];
  char host_copy[300] = "";
  int tries = 0;
  struct brokerstate *bs = local_amqp_get_a_bs(broker_id);
  if(bs->conn) return bs;
  if(SPI_connect() == SPI_ERROR_CONNECT) return NULL;
  snprintf(sql, sizeof(sql), "SELECT host, port, vhost, username, password "
                             "  FROM amqp.broker "
                             " WHERE broker_id = %d "
                             " ORDER BY host DESC, port", broker_id);
  if(SPI_OK_SELECT == SPI_execute(sql, true, 100)) {
    tries = SPI_processed;
   retry:
    tries--;
    if(SPI_processed > 0) {
      struct timeval hb = { .tv_sec = 2UL, .tv_usec = 0UL };
      amqp_rpc_reply_t *reply, s_reply;
      char *host, *vhost, *user, *pass;
      Datum port_datum;
      bool is_null;
      int port = 5672;
      bs->idx = (bs->idx + 1) % SPI_processed;
      host = SPI_getvalue(SPI_tuptable->vals[bs->idx], SPI_tuptable->tupdesc, 1);
      if(!host) host = "localhost";
      port_datum = SPI_getbinval(SPI_tuptable->vals[bs->idx], SPI_tuptable->tupdesc, 2, &is_null);
      if(!is_null) port = DatumGetInt32(port_datum);
      vhost = SPI_getvalue(SPI_tuptable->vals[bs->idx], SPI_tuptable->tupdesc, 3);
      if(!vhost) vhost = "/";
      user = SPI_getvalue(SPI_tuptable->vals[bs->idx], SPI_tuptable->tupdesc, 4);
      if(!user) user = "guest";
      pass = SPI_getvalue(SPI_tuptable->vals[bs->idx], SPI_tuptable->tupdesc, 5);
      if(!pass) pass = "guest";
      snprintf(host_copy, sizeof(host_copy), "%s:%d", host, port);

      bs->conn = amqp_new_connection();
      if(!bs->conn) { SPI_finish(); return NULL; }
      bs->sockfd = amqp_open_socket(host, port, &hb);
      if(bs->sockfd < 0) {
        elog(WARNING, "amqp[%s] login socket/connect failed: %s",
             host_copy, strerror(-bs->sockfd));
        goto busted;
      }
      amqp_set_sockfd(bs->conn, bs->sockfd);
      s_reply = amqp_login(bs->conn, vhost, 0, 131072,
                           0, AMQP_SASL_METHOD_PLAIN,
                           user, pass);
      if(s_reply.reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp[%s] login failed on broker %d", host_copy, broker_id);
        goto busted;
      }
      amqp_channel_open(bs->conn, 1);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp[%s] channel open failed on broker %d", host_copy, broker_id);
        goto busted;
      }
      amqp_channel_open(bs->conn, 2);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp[%s] channel open failed on broker %d", host_copy, broker_id);
        goto busted;
      }
      amqp_tx_select(bs->conn, 2, AMQP_EMPTY_TABLE);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp[%s] could not start tx mode on broker %d", host_copy, broker_id);
        goto busted;
      }
    } else {
      elog(WARNING, "amqp can't find broker %d", broker_id);
    }
  } else {
    elog(WARNING, "amqp broker lookup query failed");
  }
  SPI_finish();
  return bs;
 busted:
  if(tries > 0) {
    elog(WARNING, "amqp[%s] failed on trying next host", host_copy);
    goto retry;
  }
  SPI_finish();
  local_amqp_disconnect_bs(bs);
  return bs;
}
static void
local_amqp_disconnect(int broker_id) {
  struct brokerstate *bs = local_amqp_get_a_bs(broker_id);
  local_amqp_disconnect_bs(bs);
}

PG_FUNCTION_INFO_V1(pg_amqp_exchange_declare);
Datum
pg_amqp_exchange_declare(PG_FUNCTION_ARGS) {
  struct brokerstate *bs;
  if(!PG_ARGISNULL(0)) {
    int broker_id;
    broker_id = PG_GETARG_INT32(0);
    bs = local_amqp_get_bs(broker_id);
    if(bs && bs->conn) {
      amqp_rpc_reply_t *reply;
      amqp_bytes_t exchange_b;
      amqp_bytes_t exchange_type_b;
      amqp_boolean_t passive = 0;
      amqp_boolean_t durable = 0;
      amqp_boolean_t auto_delete = 0;

      set_bytes_from_text(exchange_b,1);
      set_bytes_from_text(exchange_type_b,2);
      passive = PG_GETARG_BOOL(3);
      durable = PG_GETARG_BOOL(4);
      auto_delete = PG_GETARG_BOOL(5);
      amqp_exchange_declare(bs->conn, 1,
                            exchange_b, exchange_type_b,
                            passive, durable, auto_delete, AMQP_EMPTY_TABLE);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type == AMQP_RESPONSE_NORMAL)
        PG_RETURN_BOOL(0 == 0);
      bs->inerror = 1;
    }
  }
  PG_RETURN_BOOL(0 != 0);
}
static Datum
pg_amqp_publish_opt(PG_FUNCTION_ARGS, int channel) {
  struct brokerstate *bs;
  if(!PG_ARGISNULL(0)) {
    int broker_id;
    amqp_basic_properties_t properties;
    
    int once_more = 1;
    broker_id = PG_GETARG_INT32(0);
  redo:
    bs = local_amqp_get_bs(broker_id);
    if(bs && bs->conn && (channel == 1 || !bs->inerror)) {
      int rv;
      amqp_rpc_reply_t *reply;
      amqp_boolean_t mandatory = 0;
      amqp_boolean_t immediate = 0;
      amqp_bytes_t exchange_b = amqp_cstring_bytes("amq.direct");
      amqp_bytes_t routing_key_b = amqp_cstring_bytes("");
      amqp_bytes_t body_b = amqp_cstring_bytes(""); 
      properties._flags = 0;
      
      /* Sets delivery_mode */
      if (!PG_ARGISNULL(4)) {
	  if (PG_GETARG_INT32(4) == 1 || PG_GETARG_INT32(4) == 2) {
	      properties._flags |= AMQP_BASIC_DELIVERY_MODE_FLAG;
              properties.delivery_mode = PG_GETARG_INT32(4);
	  } else {
              elog(WARNING, "Ignored delivery_mode %d, value should be 1 or 2", 
                  PG_GETARG_INT32(4));
	  }
      }

      /* Sets content_type */
      if (!PG_ARGISNULL(5)) {
	  properties._flags |= AMQP_BASIC_CONTENT_TYPE_FLAG;
	  set_bytes_from_text(properties.content_type, 5);
      }

      /* Sets reply_to */
      if (!PG_ARGISNULL(6)) {
	  properties._flags |= AMQP_BASIC_REPLY_TO_FLAG;
	  set_bytes_from_text(properties.reply_to, 6);
      }

      /* Sets correlation_id */
      if (!PG_ARGISNULL(7)) {
	  properties._flags |= AMQP_BASIC_CORRELATION_ID_FLAG;
	  set_bytes_from_text(properties.correlation_id, 7);
      }
      
      set_bytes_from_text(exchange_b,1);
      set_bytes_from_text(routing_key_b,2);
      set_bytes_from_text(body_b,3);

      if (properties._flags == 0) {
          rv = amqp_basic_publish(bs->conn, channel, exchange_b, routing_key_b,
                                  mandatory, immediate, NULL, body_b);
      } else {
          rv = amqp_basic_publish(bs->conn, channel, exchange_b, routing_key_b,
                                  mandatory, immediate, &properties, body_b);
      }

      reply = amqp_get_rpc_reply();
      if(rv || reply->reply_type != AMQP_RESPONSE_NORMAL) {
        if(once_more && (channel == 1 || bs->uncommitted == 0)) {
          once_more = 0;
          local_amqp_disconnect_bs(bs);
          goto redo;
        }
        bs->inerror = 1;
        PG_RETURN_BOOL(0 != 0);
      }
      /* channel two is transactional */
      if(channel == 2) bs->uncommitted++;
      PG_RETURN_BOOL(rv == 0);
    }
  }
  PG_RETURN_BOOL(0 != 0);
}

PG_FUNCTION_INFO_V1(pg_amqp_publish);
Datum
pg_amqp_publish(PG_FUNCTION_ARGS) {
  return pg_amqp_publish_opt(fcinfo, 2);
}

PG_FUNCTION_INFO_V1(pg_amqp_autonomous_publish);
Datum
pg_amqp_autonomous_publish(PG_FUNCTION_ARGS) {
  return pg_amqp_publish_opt(fcinfo, 1);
}

PG_FUNCTION_INFO_V1(pg_amqp_disconnect);
Datum
pg_amqp_disconnect(PG_FUNCTION_ARGS) {
  if(!PG_ARGISNULL(0)) {
    int broker_id;
    broker_id = PG_GETARG_INT32(0);
    local_amqp_disconnect(broker_id);
  }
  PG_RETURN_VOID();
}

