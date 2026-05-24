#include "net_handler.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

int send_message(int fd, const char *json) {
    uint32_t len     = (uint32_t)strlen(json);
    uint32_t net_len = htonl(len);

    if (write(fd, &net_len, 4) != 4)            return -1;
    if (write(fd, json, len) != (ssize_t)len)   return -1;
    return 0;
}

char *recv_message(int fd) {
    uint32_t net_len;
    if (read(fd, &net_len, 4) != 4) return NULL;

    uint32_t len = ntohl(net_len);
    if (len == 0 || len > 4096) return NULL;

    char *buf = malloc(len + 1);
    if (!buf) return NULL;

    ssize_t got = 0;
    while (got < (ssize_t)len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = '\0';
    return buf;
}
