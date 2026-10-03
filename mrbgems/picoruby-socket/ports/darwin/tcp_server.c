/*
 * Darwin port of picoruby-socket: Apple platforms are BSD userland, so the
 * POSIX socket implementation is used unchanged. Only TLS differs
 * (ssl_socket.c: mbedTLS instead of OpenSSL, which iOS/watchOS do not ship).
 *
 * Darwin has no MSG_NOSIGNAL, so a send to a peer that hung up raises
 * SIGPIPE and kills the process; the mruby VM cannot trap it. Every
 * accepted socket gets SO_NOSIGPIPE so that send fails with EPIPE instead.
 */
#include <sys/socket.h>

static int
darwin_accept_nosigpipe(int fd, struct sockaddr *addr, socklen_t *len)
{
  int client_fd = accept(fd, addr, len);
  if (0 <= client_fd) {
    int on = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
  return client_fd;
}
#define accept(fd, addr, len) darwin_accept_nosigpipe(fd, addr, len)

#include "../posix/tcp_server.c"
