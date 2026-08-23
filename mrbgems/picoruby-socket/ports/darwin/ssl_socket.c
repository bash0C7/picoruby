/*
 * SSL Socket implementation for Darwin cross-builds (iOS / watchOS) using
 * mbedTLS over a BSD socket.
 *
 * Apple's mobile SDKs ship no OpenSSL, so ports/posix/ssl_socket.c cannot
 * link there. This port keeps the POSIX socket model (a real fd, exposed to
 * the Ruby layer as base_socket so SSLSocket#ready? works) and runs the TLS
 * session on mbedTLS, the library picoruby-mbedtls already builds for every
 * target; its darwin port supplies the entropy (SecRandomCopyBytes).
 * Derived from ports/esp32/ssl_socket.c (the other mbedTLS-over-fd port).
 *
 * Connection states are the SOCKET_STATE_* values from socket.h, as the
 * Ruby-side polling in src/mruby/ssl_socket.c expects.
 *
 * Certificate files: MBEDTLS_FS_IO is not enabled in picoruby-mbedtls's
 * config, so the *_file setters report false; use SSLContext#ca= etc. with
 * in-memory PEM instead.
 */

#include "../../include/socket.h"
#include "picoruby.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* mbedtls includes (MBEDTLS_NET_C is enabled under PICORB_PLATFORM_POSIX) */
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

/* SSL Context structure (opaque in socket.h under PICORB_SOCKET_TLS_MBEDTLS) */
struct picorb_ssl_context {
  mbedtls_ssl_config ssl_config;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_x509_crt cacert;
  mbedtls_x509_crt cert;
  mbedtls_pk_context key;
  bool client_cert_loaded;
  bool client_key_loaded;
  int verify_mode;
};

/* SSL socket structure */
struct picorb_ssl_socket {
  picorb_socket_t *base_socket;   /* POSIX view of net_ctx.fd for Socket_ready */
  picorb_ssl_context_t *ssl_ctx;
  mbedtls_net_context net_ctx;
  mbedtls_ssl_context ssl;
  int state;                      /* SOCKET_STATE_* */
  char *hostname;
  int port;
};

/* ========================================================================
 * SSLContext Functions
 * ======================================================================== */

picorb_ssl_context_t*
SSLContext_create(picorb_state *vm)
{
  picorb_ssl_context_t *ctx = (picorb_ssl_context_t *)picorb_alloc(vm, sizeof(picorb_ssl_context_t));
  if (!ctx) return NULL;

  mbedtls_ssl_config_init(&ctx->ssl_config);
  mbedtls_entropy_init(&ctx->entropy);
  mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
  mbedtls_x509_crt_init(&ctx->cacert);
  mbedtls_x509_crt_init(&ctx->cert);
  mbedtls_pk_init(&ctx->key);
  ctx->client_cert_loaded = false;
  ctx->client_key_loaded = false;
  ctx->verify_mode = SSL_VERIFY_PEER;

  /* Seed the random number generator */
  if (mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy, NULL, 0) != 0) {
    SSLContext_free(vm, ctx);
    return NULL;
  }

  /* Setup SSL/TLS configuration */
  if (mbedtls_ssl_config_defaults(&ctx->ssl_config,
                                MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM,
                                MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
    SSLContext_free(vm, ctx);
    return NULL;
  }

  mbedtls_ssl_conf_rng(&ctx->ssl_config, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

  /* Default to verifying peer certificate */
  mbedtls_ssl_conf_authmode(&ctx->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);

  return ctx;
}

void
SSLContext_free(picorb_state *vm, picorb_ssl_context_t *ctx)
{
  if (!ctx) return;
  mbedtls_x509_crt_free(&ctx->cacert);
  mbedtls_x509_crt_free(&ctx->cert);
  mbedtls_pk_free(&ctx->key);
  mbedtls_ssl_config_free(&ctx->ssl_config);
  mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
  mbedtls_entropy_free(&ctx->entropy);
  picorb_free(vm, ctx);
}

bool
SSLContext_set_ca_file(picorb_state *vm, picorb_ssl_context_t *ctx, const char *ca_file)
{
  (void)vm; (void)ctx; (void)ca_file;
  return false;  /* no MBEDTLS_FS_IO; use SSLContext_set_ca with the PEM bytes */
}

bool
SSLContext_set_ca(picorb_state *vm, picorb_ssl_context_t *ctx, const void *addr, size_t size)
{
  (void)vm;
  if (!ctx || !addr || size == 0) return false;

  /* size + 1: mbedtls wants the terminating NUL counted for PEM input */
  int ret = mbedtls_x509_crt_parse(&ctx->cacert, (const unsigned char *)addr, size + 1);
  if (ret != 0) {
    return false;
  }
  mbedtls_ssl_conf_ca_chain(&ctx->ssl_config, &ctx->cacert, NULL);
  return true;
}

bool
SSLContext_set_cert_file(picorb_state *vm, picorb_ssl_context_t *ctx, const char *cert_file)
{
  (void)vm; (void)ctx; (void)cert_file;
  return false;  /* no MBEDTLS_FS_IO; use SSLContext_set_cert */
}

bool
SSLContext_set_cert(picorb_state *vm, picorb_ssl_context_t *ctx, const void *addr, size_t size)
{
  (void)vm;
  if (!ctx || !addr || size == 0) return false;

  int ret = mbedtls_x509_crt_parse(&ctx->cert, (const unsigned char *)addr, size + 1);
  if (ret != 0) {
    return false;
  }
  if (ctx->client_key_loaded) {
    ret = mbedtls_ssl_conf_own_cert(&ctx->ssl_config, &ctx->cert, &ctx->key);
    if (ret != 0) {
      return false;
    }
  }
  ctx->client_cert_loaded = true;
  return true;
}

bool
SSLContext_set_key_file(picorb_state *vm, picorb_ssl_context_t *ctx, const char *key_file)
{
  (void)vm; (void)ctx; (void)key_file;
  return false;  /* no MBEDTLS_FS_IO; use SSLContext_set_key */
}

bool
SSLContext_set_key(picorb_state *vm, picorb_ssl_context_t *ctx, const void *addr, size_t size)
{
  (void)vm;
  if (!ctx || !addr || size == 0) return false;

  int ret = mbedtls_pk_parse_key(&ctx->key, (const unsigned char *)addr, size + 1, NULL, 0,
                                 mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
  if (ret != 0) {
    return false;
  }
  if (ctx->client_cert_loaded) {
    ret = mbedtls_ssl_conf_own_cert(&ctx->ssl_config, &ctx->cert, &ctx->key);
    if (ret != 0) {
      return false;
    }
  }
  ctx->client_key_loaded = true;
  return true;
}

bool
SSLContext_set_verify_mode(picorb_state *vm, picorb_ssl_context_t *ctx, int mode)
{
  (void)vm;
  if (!ctx) return false;
  int mbedtls_mode;
  switch (mode) {
    case SSL_VERIFY_NONE:
      mbedtls_mode = MBEDTLS_SSL_VERIFY_NONE;
      break;
    case SSL_VERIFY_PEER:
      mbedtls_mode = MBEDTLS_SSL_VERIFY_REQUIRED;
      break;
    default:
      return false;
  }
  mbedtls_ssl_conf_authmode(&ctx->ssl_config, mbedtls_mode);
  ctx->verify_mode = mode;
  return true;
}

int
SSLContext_get_verify_mode(picorb_state *vm, picorb_ssl_context_t *ctx)
{
  (void)vm;
  if (!ctx) return -1;
  return ctx->verify_mode;
}

/* ========================================================================
 * SSLSocket Functions
 * ======================================================================== */

static void
ssl_socket_drop_base(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  if (!ssl_sock->base_socket) return;
  /* The fd belongs to net_ctx (closed by mbedtls_net_free); this is a view. */
  ssl_sock->base_socket->fd = -1;
  ssl_sock->base_socket->connected = false;
  ssl_sock->base_socket->closed = true;
  picorb_free(vm, ssl_sock->base_socket);
  ssl_sock->base_socket = NULL;
}

static void
ssl_socket_fail(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  mbedtls_net_free(&ssl_sock->net_ctx);
  ssl_socket_drop_base(vm, ssl_sock);
  ssl_sock->state = SOCKET_STATE_ERROR;
}

picorb_ssl_socket_t*
SSLSocket_create(picorb_state *vm, picorb_ssl_context_t *ssl_ctx)
{
  if (!ssl_ctx) return NULL;

  picorb_ssl_socket_t *ssl_sock = (picorb_ssl_socket_t *)picorb_alloc(vm, sizeof(picorb_ssl_socket_t));
  if (!ssl_sock) return NULL;
  memset(ssl_sock, 0, sizeof(picorb_ssl_socket_t));

  ssl_sock->ssl_ctx = ssl_ctx;
  ssl_sock->state = SOCKET_STATE_NONE;

  mbedtls_net_init(&ssl_sock->net_ctx);
  mbedtls_ssl_init(&ssl_sock->ssl);

  return ssl_sock;
}

bool
SSLSocket_set_hostname(picorb_state *vm, picorb_ssl_socket_t *ssl_sock, const char *hostname)
{
  if (!ssl_sock || !hostname) return false;
  if (ssl_sock->hostname) picorb_free(vm, ssl_sock->hostname);
  ssl_sock->hostname = (char *)picorb_alloc(vm, strlen(hostname) + 1);
  if (!ssl_sock->hostname) return false;
  strcpy(ssl_sock->hostname, hostname);
  return true;
}

bool
SSLSocket_set_port(picorb_state *vm, picorb_ssl_socket_t *ssl_sock, int port)
{
  (void)vm;
  if (!ssl_sock || port <= 0 || port > 65535) return false;
  ssl_sock->port = port;
  return true;
}

bool
SSLSocket_connect(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  if (!ssl_sock || !ssl_sock->hostname || ssl_sock->state != SOCKET_STATE_NONE) return false;

  ssl_sock->state = SOCKET_STATE_CONNECTING;
  int ret;

  if (ssl_sock->port == 0) ssl_sock->port = 443;
  char port_str[6];
  snprintf(port_str, sizeof(port_str), "%d", ssl_sock->port);

  /* 1. TCP connect (getaddrinfo + connect, blocking) */
  ret = mbedtls_net_connect(&ssl_sock->net_ctx, ssl_sock->hostname, port_str, MBEDTLS_NET_PROTO_TCP);
  if (ret != 0) {
    ssl_sock->state = SOCKET_STATE_ERROR;
    return false;
  }

  /* Expose the fd to the POSIX socket helpers (Socket_ready) */
  ssl_sock->base_socket = (picorb_socket_t *)picorb_alloc(vm, sizeof(picorb_socket_t));
  if (!ssl_sock->base_socket) {
    ssl_socket_fail(vm, ssl_sock);
    return false;
  }
  memset(ssl_sock->base_socket, 0, sizeof(picorb_socket_t));
  ssl_sock->base_socket->fd = ssl_sock->net_ctx.fd;
  ssl_sock->base_socket->family = AF_UNSPEC;
  ssl_sock->base_socket->socktype = SOCK_STREAM;
  ssl_sock->base_socket->protocol = IPPROTO_TCP;
  ssl_sock->base_socket->connected = true;
  ssl_sock->base_socket->closed = false;
  strncpy(ssl_sock->base_socket->remote_host, ssl_sock->hostname,
          sizeof(ssl_sock->base_socket->remote_host) - 1);
  ssl_sock->base_socket->remote_port = ssl_sock->port;

  /* 2. Setup SSL */
  ret = mbedtls_ssl_setup(&ssl_sock->ssl, &ssl_sock->ssl_ctx->ssl_config);
  if (ret != 0) {
    ssl_socket_fail(vm, ssl_sock);
    return false;
  }

  ret = mbedtls_ssl_set_hostname(&ssl_sock->ssl, ssl_sock->hostname);
  if (ret != 0) {
    ssl_socket_fail(vm, ssl_sock);
    return false;
  }

  mbedtls_ssl_set_bio(&ssl_sock->ssl, &ssl_sock->net_ctx, mbedtls_net_send, mbedtls_net_recv, NULL);

  /* 3. Handshake (blocking fd, so WANT_READ/WRITE only means "call again") */
  while ((ret = mbedtls_ssl_handshake(&ssl_sock->ssl)) != 0) {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
      ssl_socket_fail(vm, ssl_sock);
      return false;
    }
  }

  ssl_sock->state = SOCKET_STATE_CONNECTED;
  return true;
}

int
SSLSocket_connection_state(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  (void)vm;
  return ssl_sock ? ssl_sock->state : SOCKET_STATE_ERROR;
}

bool
SSLSocket_finish_connect(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  (void)vm;
  return ssl_sock && ssl_sock->state == SOCKET_STATE_CONNECTED;
}

picorb_socket_t*
SSLSocket_event_socket(picorb_ssl_socket_t *ssl_sock)
{
  return ssl_sock ? ssl_sock->base_socket : NULL;
}

ssize_t
SSLSocket_send(picorb_state *vm, picorb_ssl_socket_t *ssl_sock, const void *data, size_t len)
{
  (void)vm;
  if (!ssl_sock || ssl_sock->state != SOCKET_STATE_CONNECTED || !data) return -1;

  int ret = mbedtls_ssl_write(&ssl_sock->ssl, (const unsigned char *)data, len);
  if (ret < 0) {
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
      return 0; /* would block */
    }
    ssl_sock->state = SOCKET_STATE_ERROR;
    return -1;
  }
  return (ssize_t)ret;
}

ssize_t
SSLSocket_recv(picorb_state *vm, picorb_ssl_socket_t *ssl_sock, void *buf, size_t len, bool nonblock)
{
  (void)vm;
  if (!ssl_sock || ssl_sock->state != SOCKET_STATE_CONNECTED || !buf) return -1;

  if (nonblock) {
    int fd = ssl_sock->net_ctx.fd;
    int old_flags = fcntl(fd, F_GETFL, 0);
    if (old_flags == -1) return -1;
    if (fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) == -1) return -1;

    int ret = mbedtls_ssl_read(&ssl_sock->ssl, (unsigned char *)buf, len);

    if (fcntl(fd, F_SETFL, old_flags) == -1) {
      ssl_sock->state = SOCKET_STATE_ERROR;
      return -1;
    }

    if (ret > 0) return (ssize_t)ret;
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      ssl_sock->state = SOCKET_STATE_CLOSED;
      return 0;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      return PICORB_RECV_WOULD_BLOCK;
    }
    ssl_sock->state = SOCKET_STATE_ERROR;
    return -1;
  }

  /* Blocking path */
  int ret;
  for (;;) {
    ret = mbedtls_ssl_read(&ssl_sock->ssl, (unsigned char *)buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
      ssl_sock->state = SOCKET_STATE_CLOSED;
      return 0;
    }
    if (ret < 0) {
      ssl_sock->state = SOCKET_STATE_ERROR;
      return -1;
    }
    return (ssize_t)ret;
  }
}

bool
SSLSocket_close(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  if (!ssl_sock) return false;

  if (ssl_sock->state == SOCKET_STATE_CONNECTED) {
    mbedtls_ssl_close_notify(&ssl_sock->ssl);
  }
  mbedtls_net_free(&ssl_sock->net_ctx);   /* closes the fd */
  mbedtls_ssl_free(&ssl_sock->ssl);
  ssl_socket_drop_base(vm, ssl_sock);

  if (ssl_sock->hostname) {
    picorb_free(vm, ssl_sock->hostname);
    ssl_sock->hostname = NULL;
  }

  picorb_free(vm, ssl_sock);
  return true;
}

bool
SSLSocket_closed(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  (void)vm;
  return !ssl_sock || ssl_sock->state != SOCKET_STATE_CONNECTED;
}

bool
SSLSocket_ready(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  if (!ssl_sock || ssl_sock->state != SOCKET_STATE_CONNECTED) {
    return false;
  }
  /* Decrypted bytes mbedTLS already holds count as ready too; FIONREAD on
   * the fd alone (the POSIX helper) would miss them. */
  if (mbedtls_ssl_get_bytes_avail(&ssl_sock->ssl) > 0) {
    return true;
  }
  return ssl_sock->base_socket ? Socket_ready(vm, ssl_sock->base_socket) : false;
}

const char*
SSLSocket_remote_host(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  (void)vm;
  if (!ssl_sock) return NULL;
  return ssl_sock->hostname;
}

int
SSLSocket_remote_port(picorb_state *vm, picorb_ssl_socket_t *ssl_sock)
{
  (void)vm;
  if (!ssl_sock) return -1;
  return ssl_sock->port;
}
