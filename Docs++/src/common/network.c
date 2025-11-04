#include "../../include/common.h"

int create_server_socket(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket");
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    CHECK(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind");
    CHECK(listen(fd, backlog) == 0, "listen");
    return fd;
}

int create_client_socket(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket");
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    CHECK(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "connect");
    return fd;
}

int send_all(int fd, const void* buf, size_t len) {
    const char* p = (const char*)buf; size_t n = 0;
    while (n < len) {
        ssize_t k = send(fd, p + n, len - n, 0);
        if (k <= 0) return -1;
        n += (size_t)k;
    }
    return 0;
}

int recv_all(int fd, void* buf, size_t len) {
    char* p = (char*)buf; size_t n = 0;
    while (n < len) {
        ssize_t k = recv(fd, p + n, len - n, 0);
        if (k <= 0) return -1;
        n += (size_t)k;
    }
    return 0;
}

int send_u32_and_data(int fd, const void* data, uint32_t len) {
    uint32_t be = htonl(len);
    if (send_all(fd, &be, sizeof(be)) < 0) return -1;
    if (send_all(fd, data, len) < 0) return -1;
    return 0;
}

int recv_u32_and_data(int fd, char** out_data, uint32_t* out_len) {
    uint32_t be = 0;
    if (recv_all(fd, &be, sizeof(be)) < 0) return -1;
    uint32_t len = ntohl(be);
    char* buf = (char*)malloc(len + 1);
    if (!buf) return -1;
    if (recv_all(fd, buf, len) < 0) { free(buf); return -1; }
    buf[len] = '\0';
    *out_data = buf;
    *out_len = len;
    return 0;
}
