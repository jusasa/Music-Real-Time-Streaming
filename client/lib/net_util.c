/*
 * net_util.c - 소켓 통신 유틸리티 구현
 */

#include "net_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int recv_all(int fd, void *out, size_t len)
{
    uint8_t *p = (uint8_t *)out;
    size_t got = 0;

    while (got < len)
    {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n == 0)
            return -1; /* 상대방이 연결을 닫음 */
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

int connect_to_server(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        /* IP가 아니면 호스트 이름으로 해석 */
        struct hostent *he = gethostbyname(host);
        if (!he)
        {
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}
