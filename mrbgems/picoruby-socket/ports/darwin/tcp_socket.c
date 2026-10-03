/*
 * Darwin port of picoruby-socket: Apple platforms are BSD userland, so the
 * POSIX socket implementation is used unchanged. Only TLS differs
 * (ssl_socket.c: mbedTLS instead of OpenSSL, which iOS/watchOS do not ship).
 *
 * Darwin has no MSG_NOSIGNAL, so a send to a peer that hung up raises
 * SIGPIPE and kills the process; the mruby VM cannot trap it. Every client
 * socket gets SO_NOSIGPIPE so that send fails with EPIPE instead.
 */
#include <sys/socket.h>

static int
darwin_socket_nosigpipe(int domain, int type, int protocol)
{
  int fd = socket(domain, type, protocol);
  if (0 <= fd) {
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
  return fd;
}
#define socket(domain, type, protocol) darwin_socket_nosigpipe(domain, type, protocol)

#include "../posix/tcp_socket.c"
