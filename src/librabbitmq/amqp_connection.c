#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>

#include <unistd.h>
#include <sys/uio.h>

#include "amqp.h"
#include "amqp_framing.h"
#include "amqp_private.h"

#include <assert.h>


#define INITIAL_FRAME_POOL_PAGE_SIZE 65536
#define INITIAL_DECODING_POOL_PAGE_SIZE 131072
#define INITIAL_INBOUND_SOCK_BUFFER_SIZE 131072

#define ENFORCE_STATE(statevec, statenum)				\
  {									\
    amqp_connection_state_t _check_state = (statevec);			\
    int _wanted_state = (statenum);					\
    amqp_assert(_check_state->state == _wanted_state,			\
		"Programming error: invalid AMQP connection state: expected %d, got %d", \
		_wanted_state,						\
		_check_state->state);					\
  }

BIO *bio_err = NULL;


static int ssl_password_cb(char *buf, int num, int rwflag, void *data) {
  if (num < strlen((char *) data) + 1){
    return 0;
  }
  return (strlen(strcpy(buf, (char *) data)));
}

static amqp_rpc_reply_t _amqp_connection_close(amqp_connection_state_t state, int code)
{
  amqp_rpc_reply_t result;
  char codestr[13];
  snprintf(codestr, sizeof(codestr), "%d", code);
  
  /* close socket */
  if(state->sockfd >= 0){
    result = AMQP_SIMPLE_RPC(state, 0, CONNECTION, CLOSE, CLOSE_OK, amqp_connection_close_t, code, amqp_cstring_bytes(codestr), 0, 0);
    close(state->sockfd);
    state->sockfd = -1;
  }
  fprintf(stderr, "_amqp_connection_closed");
  return result;
}

static amqp_rpc_reply_t _amqp_ssl_connection_close(amqp_connection_state_t state, int code)
{
  
  amqp_rpc_reply_t result = _amqp_connection_close(state, code);
  
  if(state->ssl){
    SSL_shutdown(state->ssl);
    SSL_free(state->ssl);
    state->ssl = NULL;
  }
 
  return result;
}

int amqp_connect(amqp_connection_state_t state, char const *hostname, int portnumber, struct timeval *timeout){
  return state->connect(state, hostname, portnumber, timeout);
}

/**
 * crate native connection
 * \remark also used by ssl implementation
 * \return 1 if successful done, otherwise 0
 */
static int _amqp_connect(amqp_connection_state_t state, const char *host, int port, struct timeval *timeout) {
  int	sockfd	= -1;

  sockfd = amqp_open_socket(host, port, timeout);
  amqp_set_sockfd(state, sockfd);
  
  return (sockfd >= 0) ? 1 : 0;
}

/**
 * initialize ssl connection and verify certificate using ssl_flags
 * \return 1 if successful done, otherwise 0
 */
static int _amqp_ssl_connect(amqp_connection_state_t state, const char *host, int port, struct timeval *timeout) {
  int	success		= -1;
  int	rv 		= 0;
  int	verified	= -1;
  char	cert_cn[256]	= "\0";
  char	error[256]	= "\0";
  X509	*peerCert	= NULL;
  
  /* create socket */
  success = _amqp_connect(state, host, port, timeout);
  if(!success)
    return 0;
 
  
  state->ssl = SSL_new(state->ctx);
  state->bio = BIO_new_socket(state->sockfd, BIO_NOCLOSE);
  SSL_set_bio(state->ssl, state->bio, state->bio);

  


  do{
    
    /* do connect */
    if ((rv = SSL_connect(state->ssl)) <= 0) {
      sprintf(error, "Error during setup SSL context, rv=%d", rv);
      break;
    }
    
    /* verify the server certificate */
    if ( (state->ssl_flags & AMQP_SSL_FLAG_VERIFY) && (verified = SSL_get_verify_result(state->ssl)) != X509_V_OK) {
	sprintf(error, "SSL certificate presented by peer cannot be verified: %d\n",verified);
	break;
    }
    
    /* verify common name */
    if ( state->ssl_flags & AMQP_SSL_FLAG_CHECK_CN ) {

      /* get peer certificate */
      if (!(peerCert = SSL_get_peer_certificate(state->ssl))) {
	sprintf(error, "No SSL certificate was presented by peer");
	break;
      }

	X509_NAME_get_text_by_NID(X509_get_subject_name(peerCert), NID_commonName, cert_cn, sizeof(cert_cn));
	X509_free(peerCert);

	//TODO add wildcard support
	if (strcasecmp(cert_cn, host)) {
	  sprintf(error, "common name '%s' doesn't match host name '%s'", cert_cn, host);
	  break;
	}
      }
  }while(0);
  
  /* if an error occured, close connection */
  if(strlen(error)>0){
    fprintf(stderr, "%s\n", error);
    _amqp_ssl_connection_close(state, AMQP_REPLY_SUCCESS);
    return 0;
  }
  
  return 1;
}
  
static void _amqp_destroy_connection(amqp_connection_state_t state){
  empty_amqp_pool(&state->frame_pool);
  empty_amqp_pool(&state->decoding_pool);
  free(state->outbound_buffer.bytes);
  free(state->sock_inbound_buffer.bytes);
}

/**
 * destroy ssl connection and free ssl context
 */
static void _amqp_destroy_ssl_connection(amqp_connection_state_t state){
  _amqp_destroy_connection(state);
  
  if(state->ssl_key_password)
    free(state->ssl_key_password);
  
  /* destroy ssl context */
  if(state->ctx){
    SSL_CTX_free(state->ctx);
    state->ctx = NULL;
  }
}

amqp_connection_state_t amqp_new_ssl_connection(const char *certificate, const char *key, const char *password, const char *ca, unsigned short flags) {
  amqp_connection_state_t state;
  X509 *cert = NULL, *cacert = NULL;
  RSA *rsa = NULL;
  BIO *cbio, *kbio;
  X509_STORE *store = NULL;
  char error[254] = "\0";
  
  
  /** initialize connection_state */
  state = amqp_new_connection();
  if(!state){
    return NULL;
  }
  
  /* reset callbacks to ssl */
  state->connect = _amqp_ssl_connect;
  state->write = amqp_ssl_write;
  state->read = amqp_ssl_read;
  state->close_connection = _amqp_ssl_connection_close;
  state->destroy_connection = _amqp_destroy_ssl_connection;
  
  /* save flags for ssl */
  state->ssl_flags = flags;
  
  /**S initialize ssl */
  if(!bio_err){
      SSL_library_init();
      OpenSSL_add_all_algorithms(); 
//       SSL_load_error_strings();
      
      /* An error write context */
      bio_err = BIO_new_fp(stderr, BIO_NOCLOSE);
  }

  
  do{
    if (! (state->ctx = SSL_CTX_new(SSLv23_method()))) {
      sprintf(error, "Error during setup SSL context");
      break;
    }

    /* read certificate */
    if(!SSL_CTX_use_certificate_chain_file(state->ctx, certificate)){
      cbio = BIO_new_mem_buf((void*)certificate, -1);
      PEM_read_bio_X509(cbio, &cert, 0, NULL);
      BIO_free(cbio);

      if (!SSL_CTX_use_certificate(state->ctx, cert)) {
	sprintf(error, "Can't read certificate file");
	X509_free(cert);
	break;
      }
      X509_free(cert);
    }


    if (password != NULL) {
      state->ssl_key_password = strdup(password);
      SSL_CTX_set_default_passwd_cb_userdata(state->ctx, (void *) state->ssl_key_password);
      SSL_CTX_set_default_passwd_cb(state->ctx, ssl_password_cb);
    }
    
    /* try to read as file */
    if (!SSL_CTX_use_PrivateKey_file(state->ctx, key, SSL_FILETYPE_PEM)) {
      
      /* try reading from memory */
      kbio = BIO_new_mem_buf((void*)key, -1);
      PEM_read_bio_RSAPrivateKey(kbio, &rsa, (password) ? ssl_password_cb : NULL, state->ssl_key_password);
      BIO_free(kbio);
      if (!SSL_CTX_use_RSAPrivateKey(state->ctx, rsa)) {
	sprintf(error, "Can't read key file");
	RSA_free(rsa);
	break;
      }
      RSA_free(rsa);

    }
    
    /* load ca certificate */
    if(ca){
      if (!SSL_CTX_load_verify_locations(state->ctx, ca, 0)) {
	/* ca is not a ca file, try reading mem buffer */
	cbio = BIO_new_mem_buf((void*)ca, -1);
	PEM_read_bio_X509(cbio, &cacert, 0, NULL);
	BIO_free(cbio);
	store = SSL_CTX_get_cert_store(state->ctx);
	if(!X509_STORE_add_cert(store, cacert)){
	  fprintf(stderr, "Can't add ca file to ca store\n");
	}
	X509_free(cacert);
      }
    }
    
    /* everything was fine, return state */
    return state;
  }while(0);
  
  /* because we reached this point, an error muss be occured */
  empty_amqp_pool(&state->frame_pool);
  empty_amqp_pool(&state->decoding_pool);
  free(state);
  fprintf(stderr, "%s\n", error);
  return NULL;
  
}
  
/**
 * initialize amqp connection_state
 */
amqp_connection_state_t amqp_new_connection(void) {
  amqp_connection_state_t state = (amqp_connection_state_t) calloc(1, sizeof(struct amqp_connection_state_t_));

  if (state == NULL) {
    return NULL;
  }
  
  /* initialize with default connect method */
  state->connect = _amqp_connect;
  state->write = amqp_write;
  state->read = amqp_read;
  state->close_connection = _amqp_connection_close;
  state->destroy_connection = _amqp_destroy_connection;

  init_amqp_pool(&state->frame_pool, INITIAL_FRAME_POOL_PAGE_SIZE);
  init_amqp_pool(&state->decoding_pool, INITIAL_DECODING_POOL_PAGE_SIZE);

  state->state = CONNECTION_STATE_IDLE;

  state->inbound_buffer.bytes = NULL;
  state->outbound_buffer.bytes = NULL;
  if (amqp_tune_connection(state, 0, INITIAL_FRAME_POOL_PAGE_SIZE, 0) != 0) {
    empty_amqp_pool(&state->frame_pool);
    empty_amqp_pool(&state->decoding_pool);
    free(state);
    return NULL;
  }

  state->inbound_offset = 0;
  state->target_size = HEADER_SIZE;

  state->sockfd = -1;
  state->sock_inbound_buffer.len = INITIAL_INBOUND_SOCK_BUFFER_SIZE;
  state->sock_inbound_buffer.bytes = malloc(INITIAL_INBOUND_SOCK_BUFFER_SIZE);
  if (state->sock_inbound_buffer.bytes == NULL) {
    amqp_destroy_connection(state);
    return NULL;
  }

  state->sock_inbound_offset = 0;
  state->sock_inbound_limit = 0;

  state->first_queued_frame = NULL;
  state->last_queued_frame = NULL;

  return state;
}



int amqp_get_sockfd(amqp_connection_state_t state) {
  return state->sockfd;
}

void amqp_set_sockfd(amqp_connection_state_t state, int sockfd) {
  state->sockfd = sockfd;
}

int amqp_tune_connection(amqp_connection_state_t state,
			 int channel_max,
			 int frame_max,
			 int heartbeat)
{
  void *newbuf;

  ENFORCE_STATE(state, CONNECTION_STATE_IDLE);

  state->channel_max = channel_max;
  state->frame_max = frame_max;
  state->heartbeat = heartbeat;

  empty_amqp_pool(&state->frame_pool);
  init_amqp_pool(&state->frame_pool, frame_max);

  state->inbound_buffer.len = frame_max;
  state->outbound_buffer.len = frame_max;
  newbuf = realloc(state->outbound_buffer.bytes, frame_max);
  if (newbuf == NULL) {
    amqp_destroy_connection(state);
    return -ENOMEM;
  }
  state->outbound_buffer.bytes = newbuf;

  return 0;
}

int amqp_get_channel_max(amqp_connection_state_t state) {
  return state->channel_max;
}

void amqp_destroy_connection(amqp_connection_state_t state) {
  state->destroy_connection(state);
  free(state);
}

static void return_to_idle(amqp_connection_state_t state) {
  state->inbound_buffer.bytes = NULL;
  state->inbound_offset = 0;
  state->target_size = HEADER_SIZE;
  state->state = CONNECTION_STATE_IDLE;
}

void amqp_set_basic_return_cb(amqp_connection_state_t state, amqp_basic_return_fn_t f, void *data) {
  state->basic_return_callback = f;
  state->basic_return_callback_data = data;
}
int amqp_handle_input(amqp_connection_state_t state,
		      amqp_bytes_t received_data,
		      amqp_frame_t *decoded_frame)
{
  int total_bytes_consumed = 0;
  int bytes_consumed;

  /* Returning frame_type of zero indicates either insufficient input,
     or a complete, ignored frame was read. */
  decoded_frame->frame_type = 0;

 read_more:
  if (received_data.len == 0) {
    return total_bytes_consumed;
  }

  if (state->state == CONNECTION_STATE_IDLE) {
    state->inbound_buffer.bytes = amqp_pool_alloc(&state->frame_pool, state->inbound_buffer.len);
    state->state = CONNECTION_STATE_WAITING_FOR_HEADER;
  }

  bytes_consumed = state->target_size - state->inbound_offset;
  if (received_data.len < bytes_consumed) {
    bytes_consumed = received_data.len;
  }

  E_BYTES(state->inbound_buffer, state->inbound_offset, bytes_consumed, received_data.bytes);
  state->inbound_offset += bytes_consumed;
  total_bytes_consumed += bytes_consumed;

  assert(state->inbound_offset <= state->target_size);

  if (state->inbound_offset < state->target_size) {
    return total_bytes_consumed;
  }

  switch (state->state) {
    case CONNECTION_STATE_WAITING_FOR_HEADER:
      if (D_8(state->inbound_buffer, 0) == AMQP_PSEUDOFRAME_PROTOCOL_HEADER &&
	  D_16(state->inbound_buffer, 1) == AMQP_PSEUDOFRAME_PROTOCOL_CHANNEL)
      {
	state->target_size = 8;
	state->state = CONNECTION_STATE_WAITING_FOR_PROTOCOL_HEADER;
      } else {
	state->target_size = D_32(state->inbound_buffer, 3) + HEADER_SIZE + FOOTER_SIZE;
	state->state = CONNECTION_STATE_WAITING_FOR_BODY;
      }

      /* Wind buffer forward, and try to read some body out of it. */
      received_data.len -= bytes_consumed;
      received_data.bytes = ((char *) received_data.bytes) + bytes_consumed;
      goto read_more;

    case CONNECTION_STATE_WAITING_FOR_BODY: {
      int frame_type = D_8(state->inbound_buffer, 0);

#if 0
      printf("recving:\n");
      amqp_dump(state->inbound_buffer.bytes, state->target_size);
#endif

      /* Check frame end marker (footer) */
      if (D_8(state->inbound_buffer, state->target_size - 1) != AMQP_FRAME_END) {
	return -EINVAL;
      }

      decoded_frame->channel = D_16(state->inbound_buffer, 1);

      switch (frame_type) {
	case AMQP_FRAME_METHOD: {
	  amqp_bytes_t encoded;

	  /* Four bytes of method ID before the method args. */
	  encoded.len = state->target_size - (HEADER_SIZE + 4 + FOOTER_SIZE);
	  encoded.bytes = D_BYTES(state->inbound_buffer, HEADER_SIZE + 4, encoded.len);

	  decoded_frame->frame_type = AMQP_FRAME_METHOD;
	  decoded_frame->payload.method.id = D_32(state->inbound_buffer, HEADER_SIZE);
	  AMQP_CHECK_RESULT(amqp_decode_method(decoded_frame->payload.method.id,
					       &state->decoding_pool,
					       encoded,
					       &decoded_frame->payload.method.decoded));
	  break;
	}

	case AMQP_FRAME_HEADER: {
	  amqp_bytes_t encoded;

	  /* 12 bytes for properties header. */
	  encoded.len = state->target_size - (HEADER_SIZE + 12 + FOOTER_SIZE);
	  encoded.bytes = D_BYTES(state->inbound_buffer, HEADER_SIZE + 12, encoded.len);

	  decoded_frame->frame_type = AMQP_FRAME_HEADER;
	  decoded_frame->payload.properties.class_id = D_16(state->inbound_buffer, HEADER_SIZE);
	  decoded_frame->payload.properties.body_size = D_64(state->inbound_buffer, HEADER_SIZE+4);
	  AMQP_CHECK_RESULT(amqp_decode_properties(decoded_frame->payload.properties.class_id,
						   &state->decoding_pool,
						   encoded,
						   &decoded_frame->payload.properties.decoded));
	  break;
	}

	case AMQP_FRAME_BODY: {
	  size_t fragment_len = state->target_size - (HEADER_SIZE + FOOTER_SIZE);

	  decoded_frame->frame_type = AMQP_FRAME_BODY;
	  decoded_frame->payload.body_fragment.len = fragment_len;
	  decoded_frame->payload.body_fragment.bytes = D_BYTES(state->inbound_buffer, HEADER_SIZE, fragment_len);
	  break;
	}

	case AMQP_FRAME_HEARTBEAT:
	  decoded_frame->frame_type = AMQP_FRAME_HEARTBEAT;
	  break;

	default:
	  /* Ignore the frame by not changing frame_type away from 0. */
	  break;
      }

      return_to_idle(state);

      if(decoded_frame->frame_type == AMQP_FRAME_METHOD &&
         decoded_frame->payload.method.id == AMQP_BASIC_RETURN_METHOD) {
	  
        amqp_basic_return_t *m = decoded_frame->payload.method.decoded;
        if(state->basic_return_callback)
          state->basic_return_callback(decoded_frame->channel, m, state->basic_return_callback_data);
      }

      return total_bytes_consumed;
    }

    case CONNECTION_STATE_WAITING_FOR_PROTOCOL_HEADER:
      decoded_frame->frame_type = AMQP_PSEUDOFRAME_PROTOCOL_HEADER;
      decoded_frame->channel = AMQP_PSEUDOFRAME_PROTOCOL_CHANNEL;
      amqp_assert(D_8(state->inbound_buffer, 3) == (uint8_t) 'P', "Invalid protocol header received");
      decoded_frame->payload.protocol_header.transport_high = D_8(state->inbound_buffer, 4);
      decoded_frame->payload.protocol_header.transport_low = D_8(state->inbound_buffer, 5);
      decoded_frame->payload.protocol_header.protocol_version_major = D_8(state->inbound_buffer, 6);
      decoded_frame->payload.protocol_header.protocol_version_minor = D_8(state->inbound_buffer, 7);

      return_to_idle(state);
      return total_bytes_consumed;

    default:
      amqp_assert(0, "Internal error: invalid amqp_connection_state_t->state %d", state->state);
  }
}

amqp_boolean_t amqp_release_buffers_ok(amqp_connection_state_t state) {
  return (state->state == CONNECTION_STATE_IDLE) && (state->first_queued_frame == NULL);
}

void amqp_release_buffers(amqp_connection_state_t state) {
  ENFORCE_STATE(state, CONNECTION_STATE_IDLE);

  amqp_assert(state->first_queued_frame == NULL,
	      "Programming error: attempt to amqp_release_buffers while waiting events enqueued");

  recycle_amqp_pool(&state->frame_pool);
  recycle_amqp_pool(&state->decoding_pool);
}

void amqp_maybe_release_buffers(amqp_connection_state_t state) {
  if (amqp_release_buffers_ok(state)) {
    amqp_release_buffers(state);
  }
}

static int inner_send_frame(amqp_connection_state_t state,
			    amqp_frame_t const *frame,
			    amqp_bytes_t *encoded,
			    int *payload_len)
{
  int separate_body;

  E_8(state->outbound_buffer, 0, frame->frame_type);
  E_16(state->outbound_buffer, 1, frame->channel);
  switch (frame->frame_type) {
    case AMQP_FRAME_METHOD:
      E_32(state->outbound_buffer, HEADER_SIZE, frame->payload.method.id);
      encoded->len = state->outbound_buffer.len - (HEADER_SIZE + 4 + FOOTER_SIZE);
      encoded->bytes = D_BYTES(state->outbound_buffer, HEADER_SIZE + 4, encoded->len);
      *payload_len = AMQP_CHECK_RESULT(amqp_encode_method(frame->payload.method.id,
							  frame->payload.method.decoded,
							  *encoded)) + 4;
      separate_body = 0;
      break;

    case AMQP_FRAME_HEADER:
      E_16(state->outbound_buffer, HEADER_SIZE, frame->payload.properties.class_id);
      E_16(state->outbound_buffer, HEADER_SIZE+2, 0); /* "weight" */
      E_64(state->outbound_buffer, HEADER_SIZE+4, frame->payload.properties.body_size);
      encoded->len = state->outbound_buffer.len - (HEADER_SIZE + 12 + FOOTER_SIZE);
      encoded->bytes = D_BYTES(state->outbound_buffer, HEADER_SIZE + 12, encoded->len);
      *payload_len = AMQP_CHECK_RESULT(amqp_encode_properties(frame->payload.properties.class_id,
							      frame->payload.properties.decoded,
							      *encoded)) + 12;
      separate_body = 0;
      break;

    case AMQP_FRAME_BODY:
      *encoded = frame->payload.body_fragment;
      *payload_len = encoded->len;
      separate_body = 1;
      break;

    case AMQP_FRAME_HEARTBEAT:
      *encoded = AMQP_EMPTY_BYTES;
      *payload_len = 0;
      separate_body = 0;
      break;

    default:
      return -EINVAL;
  }

  E_32(state->outbound_buffer, 3, *payload_len);
  if (!separate_body) {
    E_8(state->outbound_buffer, *payload_len + HEADER_SIZE, AMQP_FRAME_END);
  }

#if 0
  if (separate_body) {
    printf("sending body frame (header):\n");
    amqp_dump(state->outbound_buffer.bytes, HEADER_SIZE);
    printf("sending body frame (payload):\n");
    amqp_dump(encoded->bytes, *payload_len);
  } else {
    printf("sending:\n");
    amqp_dump(state->outbound_buffer.bytes, *payload_len + HEADER_SIZE + FOOTER_SIZE);
  }
#endif

  return separate_body;
}

int amqp_send_frame(amqp_connection_state_t state,
		    amqp_frame_t const *frame)
{
  amqp_bytes_t encoded;
  int payload_len;
  int separate_body;

  separate_body = inner_send_frame(state, frame, &encoded, &payload_len);
  switch (separate_body) {
    case 0:
      AMQP_CHECK_RESULT(state->write(state, state->outbound_buffer.bytes, payload_len + (HEADER_SIZE + FOOTER_SIZE)));
      return 0;

    case 1:
      AMQP_CHECK_RESULT(state->write(state, state->outbound_buffer.bytes, HEADER_SIZE));
      AMQP_CHECK_RESULT(state->write(state, encoded.bytes, payload_len));
      {
	unsigned char frame_end_byte = AMQP_FRAME_END;
	assert(FOOTER_SIZE == 1);
	AMQP_CHECK_RESULT(state->write(state, &frame_end_byte, FOOTER_SIZE));
      }
      return 0;

    default:
      return separate_body;
  }
}

int amqp_send_frame_to(amqp_connection_state_t state,
		       amqp_frame_t const *frame,
		       amqp_output_fn_t fn,
		       void *context)
{
  amqp_bytes_t encoded;
  int payload_len;
  int separate_body;

  separate_body = inner_send_frame(state, frame, &encoded, &payload_len);
  switch (separate_body) {
    case 0:
      AMQP_CHECK_RESULT(fn(context,
			   state->outbound_buffer.bytes,
			   payload_len + (HEADER_SIZE + FOOTER_SIZE)));
      return 0;

    case 1:
      AMQP_CHECK_RESULT(fn(context, state->outbound_buffer.bytes, HEADER_SIZE));
      AMQP_CHECK_RESULT(fn(context, encoded.bytes, payload_len));
      {
	unsigned char frame_end_byte = AMQP_FRAME_END;
	assert(FOOTER_SIZE == 1);
	AMQP_CHECK_RESULT(fn(context, &frame_end_byte, FOOTER_SIZE));
      }
      return 0;

    default:
      return separate_body;
  }
}

int amqp_write(amqp_connection_state_t state, void *buffer, size_t size) {
  return write(state->sockfd, buffer, size);
}
int amqp_ssl_write(amqp_connection_state_t state, void *buffer, size_t size) {
  int	err;
  int	len;

  len = SSL_write(state->ssl, buffer, size);

  if (! len) {
    switch ((err = SSL_get_error(state->ssl, len))) {
      case SSL_ERROR_SYSCALL:
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
          case SSL_ERROR_WANT_WRITE:
          case SSL_ERROR_WANT_READ:
            errno = EWOULDBLOCK;
            return (0);
        }

      case SSL_ERROR_SSL:
        if (errno == EAGAIN) {
          return 0;
        }

      default:
        return 0;
    }
  }


  return len;
}
int amqp_read(amqp_connection_state_t state, void *buffer, size_t size) {
  return read(state->sockfd, buffer, size);
}

int amqp_ssl_read(amqp_connection_state_t state, void *buffer, size_t size) {
  int	err;
  int	len;

  len = SSL_read(state->ssl, buffer, size);

  if (!len) {
    switch ((err = SSL_get_error(state->ssl, len))) {
      case SSL_ERROR_SYSCALL:
        if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR)) {
          case SSL_ERROR_WANT_READ:
            errno = EWOULDBLOCK;
            return 0;
        }

      case SSL_ERROR_SSL:
        if (errno == EAGAIN) {
          return 0;
        }

      default:
        return 0;
    }
  }


  return len;
}
// kate: indent-width 2; replace-tabs off; indent-mode cstyle; auto-insert-doxygen on; line-numbers on; tab-indents on; keep-extra-spaces off; auto-brackets on;
