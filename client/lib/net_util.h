/*
 * net_util.h - 소켓 통신 유틸리티
 */

#ifndef CLIENT_LIB_NET_UTIL_H
#define CLIENT_LIB_NET_UTIL_H

#include <stddef.h>

/*
 * 소켓에서 정확히 len 바이트를 읽는다.
 * 성공 시 0, 연결 종료/오류 시 -1
 */
int recv_all(int fd, void *out, size_t len);

/*
 * host:port 로 TCP 연결을 맺고 소켓 fd를 반환한다.
 * 실패 시 -1
 */
int connect_to_server(const char *host, int port);

#endif /* CLIENT_LIB_NET_UTIL_H */
