/*
 * Darwin port of picoruby-socket: Apple platforms are BSD userland, so the
 * POSIX socket implementation is used unchanged. Only TLS differs
 * (ssl_socket.c: mbedTLS instead of OpenSSL, which iOS/watchOS do not ship).
 */
#include "../posix/udp_socket.c"
