#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "../../include/common.h"
#include "file_registry.h"

typedef struct {
    char ip[64];
    int ctrl_port;
    int client_port;
} SSInfo;

static SSInfo g_ss; // single SS for prototype
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void handle_client_conn(int connfd, struct sockaddr_in* addr) {
    (void)addr;
    while (1) {
        char* data = NULL; uint32_t len = 0;
        if (recv_u32_and_data(connfd, &data, &len) < 0) { break; }
        // Copy before tokenizing
        char* cmd = strndup(data, len);
        free(data);
        if (!cmd) { break; }
        // Parse
        char* saveptr=NULL; char* tok = strtok_r(cmd, " \t\n", &saveptr);
        if (!tok) { send_u32_and_data(connfd, "ERR empty", 9); free(cmd); continue; }
        if (strcmp(tok, "REGISTER_SS") == 0) {
            // REGISTER_SS <ctrl_port> <client_port>
            char* s_ctrl = strtok_r(NULL, " \t\n", &saveptr);
            char* s_cli  = strtok_r(NULL, " \t\n", &saveptr);
            if (!s_ctrl || !s_cli) {
                const char* e = "ERR usage REGISTER_SS <ctrl> <client>";
                send_u32_and_data(connfd, e, (uint32_t)strlen(e));
            } else {
                pthread_mutex_lock(&g_lock);
                strncpy(g_ss.ip, "127.0.0.1", sizeof(g_ss.ip)-1); // assuming localhost for prototype
                g_ss.ctrl_port = atoi(s_ctrl);
                g_ss.client_port = atoi(s_cli);
                pthread_mutex_unlock(&g_lock);
                // Query SS for current files and update registry
                int s = create_client_socket(g_ss.ip, g_ss.ctrl_port);
                if (s >= 0) {
                    struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
                    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    const char* lcmd = "LIST_FILES"; send_u32_and_data(s, lcmd, (uint32_t)strlen(lcmd));
                    char* lresp=NULL; uint32_t llen=0;
                    if (recv_u32_and_data(s, &lresp, &llen) == 0) {
                        if (llen>=2 && strncmp(lresp, "OK", 2)==0) {
                            // parse count header, then lines of file names
                            char* dup = strndup(lresp, llen);
                            char* sp=NULL; (void)strtok_r(dup, "\n", &sp); // header
                            char* line=NULL;
                            while ((line = strtok_r(NULL, "\n", &sp)) != NULL) {
                                if (*line) registry_add(line, "ss0");
                            }
                            free(dup);
                        }
                        free(lresp);
                    }
                    close(s);
                }
                const char* ok = "OK registered"; send_u32_and_data(connfd, ok, (uint32_t)strlen(ok));
            }
        } else if (strcmp(tok, "VIEW") == 0) {
            (void)strtok_r(NULL, "\n", &saveptr); // ignore flags for now
            pthread_mutex_lock(&g_lock);
            size_t n = registry_used();
            size_t cap = 128 + n * 256; char* buf = (char*)malloc(cap); buf[0]='\0';
            snprintf(buf, cap, "OK %zu\n", n);
            for (size_t i=0;i<n;i++) {
                const FileEntry* e = registry_entry(i);
                if (e && e->name) {
                    strncat(buf, e->name, cap-1 - strlen(buf));
                    strncat(buf, "\n", cap-1 - strlen(buf));
                }
            }
            pthread_mutex_unlock(&g_lock);
            send_u32_and_data(connfd, buf, (uint32_t)strlen(buf));
            free(buf);
        } else if (strcmp(tok, "LOCATE") == 0) {
            char* fname = strtok_r(NULL, " \t\n", &saveptr);
            if (!fname) {
                const char* e = "ERR usage LOCATE <filename>";
                send_u32_and_data(connfd, e, (uint32_t)strlen(e));
            } else {
                pthread_mutex_lock(&g_lock);
                const char* ss_id = registry_lookup(fname);
                SSInfo ss = g_ss;
                pthread_mutex_unlock(&g_lock);
                if (!ss_id) {
                    const char* e = "ERR notfound";
                    send_u32_and_data(connfd, e, (uint32_t)strlen(e));
                } else {
                    char line[128]; snprintf(line, sizeof(line), "OK %s %d\n", ss.ip, ss.client_port);
                    send_u32_and_data(connfd, line, (uint32_t)strlen(line));
                }
            }
        } else if (strcmp(tok, "CREATE") == 0) {
            char* fname = strtok_r(NULL, " \t\n", &saveptr);
            char* owner = strtok_r(NULL, " \t\n", &saveptr);
            if (!fname || !owner) {
                const char* e = "ERR usage CREATE <filename> <owner>";
                send_u32_and_data(connfd, e, (uint32_t)strlen(e));
            } else {
                pthread_mutex_lock(&g_lock);
                SSInfo ss = g_ss;
                pthread_mutex_unlock(&g_lock);
                if (ss.client_port == 0) {
                    const char* e = "ERR no_ss";
                    send_u32_and_data(connfd, e, (uint32_t)strlen(e));
                } else {
                    int s = create_client_socket(ss.ip, ss.ctrl_port);
                    if (s < 0) {
                        const char* e = "ERR ss_unavail";
                        send_u32_and_data(connfd, e, (uint32_t)strlen(e));
                    } else {
                        char cmdline[512]; snprintf(cmdline, sizeof(cmdline), "CREATE %s %s", fname, owner);
                        send_u32_and_data(s, cmdline, (uint32_t)strlen(cmdline));
                        // Set a 3s receive timeout so client doesn't hang indefinitely if SS is unresponsive
                        struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
                        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        char* resp=NULL; uint32_t rlen=0;
                        if (recv_u32_and_data(s, &resp, &rlen) < 0) {
                            const char* e = "ERR ss_noresp"; send_u32_and_data(connfd, e, (uint32_t)strlen(e));
                        } else {
                            if (strncmp(resp, "OK", 2) == 0) {
                                registry_add(fname, "ss0");
                                const char* ok = "OK created"; send_u32_and_data(connfd, ok, (uint32_t)strlen(ok));
                            } else {
                                send_u32_and_data(connfd, resp, rlen);
                            }
                            free(resp);
                        }
                        close(s);
                    }
                }
            }
        } else if (strcmp(tok, "INFO") == 0) {
            char* fname = strtok_r(NULL, " \t\n", &saveptr);
            if (!fname) { const char* e = "ERR usage INFO <filename>"; send_u32_and_data(connfd, e, (uint32_t)strlen(e)); }
            else {
                const char* ok = "OK info_stub"; send_u32_and_data(connfd, ok, (uint32_t)strlen(ok));
            }
        } else {
            const char* e = "ERR unknown"; send_u32_and_data(connfd, e, (uint32_t)strlen(e));
        }
        free(cmd);
    }
    close(connfd);
}

static void* accept_loop(void* arg) {
    int fd = *(int*)arg;
    while (1) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int c = accept(fd, (struct sockaddr*)&addr, &alen);
        if (c < 0) continue;
        pthread_t th; pthread_create(&th, NULL, (void*(*)(void*))handle_client_conn, (void*)(long)c);
        pthread_detach(th);
    }
    return NULL;
}

typedef struct { int fd; struct sockaddr_in addr; } ConnArg;
static void* conn_thread(void* arg){
    ConnArg* ca = (ConnArg*)arg; int fd = ca->fd; struct sockaddr_in addr = ca->addr; free(ca);
    handle_client_conn(fd, &addr); return NULL;
}

int main() {
    const char* env = getenv("NS_PORT");
    int port = env ? atoi(env) : 9000;
    registry_init();
    memset(&g_ss, 0, sizeof(g_ss));
    printf("NameServer listening on %d...\n", port);
    int fd = create_server_socket(port, 128);
    // main accept loop: spawn a thread per connection to allow multiple clients
    while (1) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int connfd = accept(fd, (struct sockaddr*)&addr, &alen);
        if (connfd < 0) continue;
        ConnArg* ca = (ConnArg*)malloc(sizeof(ConnArg)); ca->fd = connfd; ca->addr = addr;
        pthread_t th; pthread_create(&th, NULL, conn_thread, ca); pthread_detach(th);
    }
    return 0;
}
