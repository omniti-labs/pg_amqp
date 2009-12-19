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

#include <time.h>
#include <sys/time.h>

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

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif
void _PG_init(void);
Datum pg_amqp_publish(PG_FUNCTION_ARGS);
Datum pg_amqp_disconnect(PG_FUNCTION_ARGS);

struct brokerstate {
  int broker_id;
  amqp_connection_state_t conn;
  int sockfd;
  int uncommitted;
  struct brokerstate *next;
};

static struct brokerstate *HEAD_BS = NULL;

static void
local_amqp_disconnect_bs(struct brokerstate *bs) {
  if(bs && bs->conn) {
    amqp_connection_close(bs->conn, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(bs->conn);
    memset(bs, 0, sizeof(*bs));
  }
}
static void amqp_local_phase2(XactEvent event, void *arg) {
  amqp_rpc_reply_t *reply;
  struct brokerstate *bs;
  switch(event) {
    case XACT_EVENT_COMMIT:
      for(bs = HEAD_BS; bs; bs = bs->next) {
        if(!bs->uncommitted) continue;
        amqp_tx_commit(bs->conn, 1, AMQP_EMPTY_TABLE);
        reply = amqp_get_rpc_reply();
        if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
          elog(WARNING, "amqp could not commit tx mode on broker %d", bs->broker_id);
          local_amqp_disconnect_bs(bs);
        }
        bs->uncommitted = 0;
      }
      break;
    case XACT_EVENT_ABORT:
      for(bs = HEAD_BS; bs; bs = bs->next) {
        if(!bs->uncommitted) continue;
        amqp_tx_rollback(bs->conn, 1, AMQP_EMPTY_TABLE);
        reply = amqp_get_rpc_reply();
        if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
          elog(WARNING, "amqp could not commit tx mode on broker %d", bs->broker_id);
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
local_amqp_get_a_bs(broker_id) {
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
local_amqp_get_bs(broker_id) {
  char sql[1024];
  struct brokerstate *bs = local_amqp_get_a_bs(broker_id);
  if(bs->conn) return bs;
  if(SPI_connect() == SPI_ERROR_CONNECT) return NULL;
  snprintf(sql, sizeof(sql), "SELECT host, port, vhost, username, password "
                             "  FROM amqp.broker "
                             " WHERE broker_id = %d", broker_id);
  if(SPI_OK_SELECT == SPI_execute(sql, true, 1)) {
    if(1 == SPI_processed) {
      amqp_rpc_reply_t *reply, s_reply;
      char *host, *vhost, *user, *pass;
      Datum port_datum;
      bool is_null;
      int port = 5672;
      host = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
      if(!host) host = "localhost";
      port_datum = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &is_null);
      if(!is_null) port = DatumGetInt32(port_datum);
      vhost = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3);
      if(!vhost) vhost = "/";
      user = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 4);
      if(!user) user = "guest";
      pass = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 5);
      if(!pass) pass = "guest";
      SPI_finish();

      bs->conn = amqp_new_connection();
      if(!bs->conn) return NULL;
      bs->sockfd = amqp_open_socket(host, port);
      if(bs->sockfd < 0) goto busted;
      amqp_set_sockfd(bs->conn, bs->sockfd);
      s_reply = amqp_login(bs->conn, vhost, 0, 131072,
                           0, AMQP_SASL_METHOD_PLAIN,
                           user, pass);
      if(s_reply.reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp login failed on broker %d", broker_id);
        goto busted;
      }
      amqp_channel_open(bs->conn, 1);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp channel open failed on broker %d", broker_id);
        goto busted;
      }
      amqp_tx_select(bs->conn, 1, AMQP_EMPTY_TABLE);
      reply = amqp_get_rpc_reply();
      if(reply->reply_type != AMQP_RESPONSE_NORMAL) {
        elog(WARNING, "amqp could not start tx mode on broker %d", broker_id);
        goto busted;
      }
    } else {
      elog(WARNING, "amqp can't find broker %d", broker_id);
      SPI_finish();
    }
  } else {
    elog(WARNING, "amqp broker lookup query failed");
    SPI_finish();
  }
  return bs;
 busted:
  local_amqp_disconnect_bs(bs);
  return bs;
}
static void
local_amqp_disconnect(broker_id) {
  struct brokerstate *bs = local_amqp_get_a_bs(broker_id);
  local_amqp_disconnect_bs(bs);
}

PG_FUNCTION_INFO_V1(pg_amqp_publish);
Datum
pg_amqp_publish(PG_FUNCTION_ARGS) {
  struct brokerstate *bs;
  if(!PG_ARGISNULL(0)) {
    int broker_id;
    broker_id = PG_GETARG_INT32(0);
    bs = local_amqp_get_bs(broker_id);
    if(bs && bs->conn) {
      int rv;
      amqp_rpc_reply_t *reply;
      amqp_boolean_t mandatory = 0;
      amqp_boolean_t immediate = 0;
      amqp_bytes_t exchange_b = amqp_cstring_bytes("amq.direct");
      amqp_bytes_t routing_key_b = amqp_cstring_bytes("");
      amqp_bytes_t body_b = amqp_cstring_bytes("");

#define set_bytes_from_test(var,col) do { \
  if(!PG_ARGISNULL(col)) { \
    text *txt = PG_GETARG_TEXT_PP(col); \
    var.bytes = VARDATA_ANY(txt); \
    var.len = VARSIZE_ANY_EXHDR(txt); \
  } \
} while(0)
      set_bytes_from_test(exchange_b,1);
      set_bytes_from_test(routing_key_b,2);
      set_bytes_from_test(body_b,3);
      rv = amqp_basic_publish(bs->conn, 1, exchange_b, routing_key_b,
                              mandatory, immediate, NULL, body_b);
      reply = amqp_get_rpc_reply();
      if(rv || reply->reply_type != AMQP_RESPONSE_NORMAL) PG_RETURN_BOOL(0 != 0);
      bs->uncommitted++;
      PG_RETURN_BOOL(rv == 0);
    }
  }
  PG_RETURN_BOOL(0 != 0);
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

