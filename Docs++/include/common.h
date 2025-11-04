#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#define CHECK(x, msg) do { if (!(x)) { perror(msg); exit(EXIT_FAILURE); } } while(0)

int create_server_socket(int port, int backlog);
int create_client_socket(const char* ip, int port);
int send_all(int fd, const void* buf, size_t len);
int recv_all(int fd, void* buf, size_t len);
int send_u32_and_data(int fd, const void* data, uint32_t len);
int recv_u32_and_data(int fd, char** out_data, uint32_t* out_len);

#endif // COMMON_H
