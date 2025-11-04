#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include "../../include/common.h"
#include "file_ops.h"

static const char* kDataRoot = "ss_data";
static const char* kFilesDir = "ss_data/files";
static const char* kUndoDir  = "ss_data/undo";

static void ensure_dirs() {
    // Create ss_data and ss_data/files if they don't exist
    mkdir(kDataRoot, 0755);
    mkdir(kFilesDir, 0755);
    mkdir(kUndoDir, 0755);
}

static void make_filepath(const char* fname, char* out, size_t outsz) {
    // Very simple sanitization: disallow slashes
    char clean[256]; size_t j=0; for (size_t i=0; fname[i] && j<sizeof(clean)-1; ++i) {
        if (fname[i] == '/' || fname[i] == '\\') {
            continue;
        }
        clean[j++] = fname[i];
    }
    clean[j] = '\0';
    snprintf(out, outsz, "%s/%s", kFilesDir, clean);
}

static void* ctrl_accept_loop(void* arg) {
    int fd = *(int*)arg;
    while (1) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int c = accept(fd, (struct sockaddr*)&addr, &alen);
        if (c < 0) continue;
        // Read a command line
        char* data=NULL; uint32_t len=0;
        if (recv_u32_and_data(c, &data, &len) < 0) { close(c); continue; }
        if (len >= 6 && strncmp(data, "CREATE", 6)==0) {
            // CREATE <filename> <owner>
            char* cmd = strndup(data, len);
            char* save=NULL; (void)strtok_r(cmd, " \t\n", &save); // CREATE
            char* fname = strtok_r(NULL, " \t\n", &save);
            // owner currently unused for persistence
            (void)strtok_r(NULL, " \t\n", &save);
            if (!fname) {
                const char* e = "ERR usage"; send_u32_and_data(c, e, (uint32_t)strlen(e));
            } else {
                char path[512]; make_filepath(fname, path, sizeof(path));
                FILE* f = fopen(path, "r");
                if (f) { // already exists
                    fclose(f);
                    const char* e = "ERR exists"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                } else {
                    f = fopen(path, "w");
                    if (!f) {
                        const char* e = "ERR create"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                    } else {
                        fflush(f); fsync(fileno(f)); fclose(f);
                        const char* ok = "OK"; send_u32_and_data(c, ok, (uint32_t)strlen(ok));
                    }
                }
            }
            free(cmd);
        } else if (len >= 10 && strncmp(data, "LIST_FILES", 10)==0) {
            // Enumerate files in ss_data/files
            DIR* d = opendir(kFilesDir);
            if (!d) { const char* e = "ERR list"; send_u32_and_data(c, e, (uint32_t)strlen(e)); }
            else {
                // Collect names
                size_t cap = 1024; char* out = (char*)malloc(cap); out[0]='\0';
                size_t count = 0; struct dirent* ent;
                while ((ent = readdir(d)) != NULL) {
                    if (strcmp(ent->d_name, ".")==0 || strcmp(ent->d_name, "..")==0) continue;
                    // Simple filter: skip non-regular if d_type available but not portable; assume files here
                    size_t need = strlen(ent->d_name) + 2; // name + \n + maybe header expansion later
                    if (strlen(out) + need + 32 >= cap) { cap *= 2; out = (char*)realloc(out, cap); }
                    strcat(out, ent->d_name); strcat(out, "\n"); count++;
                }
                closedir(d);
                // Prepend header OK <count>\n
                char header[64]; snprintf(header, sizeof(header), "OK %zu\n", count);
                size_t total = strlen(header) + strlen(out);
                char* msg = (char*)malloc(total + 1);
                strcpy(msg, header); strcat(msg, out);
                send_u32_and_data(c, msg, (uint32_t)strlen(msg));
                free(out); free(msg);
            }
        } else {
            const char* e = "ERR"; send_u32_and_data(c, e, (uint32_t)strlen(e));
        }
        free(data); close(c);
    }
    return NULL;
}

static char* read_entire_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)sz + 1); if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[n] = '\0'; if (out_len) *out_len = n; return buf;
}

typedef struct LockEntry {
    char* fname;
    int sentence_idx; // 1-based
    int owner_fd;
    struct LockEntry* next;
} LockEntry;

typedef struct WriteSession {
    char* fname;
    int sentence_idx;
    char* new_sentence; // set after WRITE_SET
    struct WriteSession* next;
    int fd;
} WriteSession;

static LockEntry* g_locks = NULL; // guarded by g_state_mu
static WriteSession* g_sessions = NULL; // guarded by g_state_mu
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;

static int lock_acquire(const char* fname, int idx, int fd) {
    pthread_mutex_lock(&g_state_mu);
    for (LockEntry* e=g_locks; e; e=e->next) {
        if (e->sentence_idx==idx && strcmp(e->fname, fname)==0) { pthread_mutex_unlock(&g_state_mu); return -1; }
    }
    LockEntry* ne = (LockEntry*)calloc(1, sizeof(LockEntry));
    ne->fname = strdup(fname); ne->sentence_idx = idx; ne->owner_fd = fd; ne->next = g_locks; g_locks = ne;
    pthread_mutex_unlock(&g_state_mu);
    return 0;
}
static void lock_release_by_fd(int fd) {
    pthread_mutex_lock(&g_state_mu);
    LockEntry** pp = &g_locks; while (*pp) {
        if ((*pp)->owner_fd == fd) { LockEntry* del=*pp; *pp=del->next; free(del->fname); free(del); }
        else pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_state_mu);
}

static WriteSession* session_get(int fd) {
    for (WriteSession* s=g_sessions; s; s=s->next) {
        if (s->fd==fd) return s;
    }
    return NULL;
}
static WriteSession* session_start(int fd, const char* fname, int idx) {
    pthread_mutex_lock(&g_state_mu);
    if (session_get(fd)) { pthread_mutex_unlock(&g_state_mu); return NULL; }
    WriteSession* s = (WriteSession*)calloc(1, sizeof(WriteSession));
    s->fd=fd; s->fname=strdup(fname); s->sentence_idx=idx; s->next=g_sessions; g_sessions=s;
    pthread_mutex_unlock(&g_state_mu);
    return s;
}
static void session_set_text(int fd, const char* text) {
    pthread_mutex_lock(&g_state_mu);
    WriteSession* s = session_get(fd);
    if (s) { free(s->new_sentence); s->new_sentence = strdup(text); }
    pthread_mutex_unlock(&g_state_mu);
}
static WriteSession* session_take(int fd) {
    pthread_mutex_lock(&g_state_mu);
    WriteSession** pp=&g_sessions; while (*pp) {
        if ((*pp)->fd==fd) { WriteSession* s=*pp; *pp=s->next; pthread_mutex_unlock(&g_state_mu); return s; }
        pp=&(*pp)->next;
    }
    pthread_mutex_unlock(&g_state_mu);
    return NULL;
}

static int write_file_atomic(const char* path, const char* data, size_t len) {
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb"); if (!f) return -1;
    if (fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void* client_accept_loop(void* arg) {
    int fd = *(int*)arg;
    while (1) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int c = accept(fd, (struct sockaddr*)&addr, &alen);
        if (c < 0) continue;
        while (1) {
            char* data=NULL; uint32_t len=0;
            if (recv_u32_and_data(c, &data, &len) < 0) { break; }
            if (len >= 4 && strncmp(data, "READ", 4)==0) {
                // READ <filename>
                char* cmd = strndup(data, len);
                char* save=NULL; (void)strtok_r(cmd, " \t\n", &save);
                char* fname = strtok_r(NULL, " \t\n", &save);
                if (!fname) {
                    const char* e = "ERR usage"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                } else {
                    char path[512]; make_filepath(fname, path, sizeof(path));
                    size_t flen=0; char* content = read_entire_file(path, &flen);
                    if (!content) {
                        const char* e = "ERR notfound"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                    } else {
                        size_t hdrlen = 3; // OK\n
                        char* out = (char*)malloc(hdrlen + flen + 1);
                        memcpy(out, "OK\n", hdrlen);
                        memcpy(out+hdrlen, content, flen);
                        out[hdrlen+flen] = '\0';
                        send_u32_and_data(c, out, (uint32_t)(hdrlen+flen));
                        free(out);
                        free(content);
                    }
                }
                free(cmd);
            } else if (len >= 11 && strncmp(data, "WRITE_BEGIN", 11)==0) {
                // WRITE_BEGIN <filename> <idx>
                char* cmd = strndup(data, len);
                char* save=NULL; (void)strtok_r(cmd, " \t\n", &save);
                char* fname = strtok_r(NULL, " \t\n", &save);
                char* sidx  = strtok_r(NULL, " \t\n", &save);
                if (!fname || !sidx) {
                    const char* e = "ERR usage"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                } else {
                    int idx = atoi(sidx);
                    // try lock
                    if (lock_acquire(fname, idx, c) != 0) {
                        const char* e = "ERR locked"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                    } else {
                        if (!session_start(c, fname, idx)) {
                            const char* e = "ERR session"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                            lock_release_by_fd(c);
                        } else {
                            const char* ok = "OK begin"; send_u32_and_data(c, ok, (uint32_t)strlen(ok));
                        }
                    }
                }
                free(cmd);
            } else if (len >= 10 && strncmp(data, "WRITE_SET ", 10)==0) {
                // WRITE_SET <new sentence text>
                const char* text = data + 10;
                WriteSession* s = session_get(c);
                if (!s) {
                    const char* e = "ERR nosession"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                } else {
                    session_set_text(c, text);
                    const char* ok = "OK set"; send_u32_and_data(c, ok, (uint32_t)strlen(ok));
                }
            } else if (len >= 12 && strncmp(data, "WRITE_COMMIT", 12)==0) {
                // Commit change
                WriteSession* s = session_get(c);
                if (!s || !s->new_sentence) {
                    const char* e = "ERR nosession"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                } else {
                    // Load file, parse, edit, join, persist
                    char path[512]; make_filepath(s->fname, path, sizeof(path));
                    size_t flen=0; char* content = read_entire_file(path, &flen);
                    if (!content) { const char* e = "ERR notfound"; send_u32_and_data(c, e, (uint32_t)strlen(e)); }
                    else {
                        FileContent fc = parse_sentences(content);
                        // Expand if idx == fc.count+1 (append), else require 1..count
                        int idx = s->sentence_idx;
                        if (idx < 1 || idx > (int)fc.count + 1) {
                            const char* e = "ERR badindex"; send_u32_and_data(c, e, (uint32_t)strlen(e));
                        } else {
                            if (idx == (int)fc.count + 1) {
                                // append; ensure sentence ends with punctuation and a space for continuity
                                const char* txt = s->new_sentence;
                                size_t n = strlen(txt);
                                int needs_punct = (n==0 || (txt[n-1] != '.' && txt[n-1] != '!' && txt[n-1] != '?'));
                                size_t extra = needs_punct ? 1 : 0;
                                char* sen = (char*)malloc(n + extra + 1);
                                memcpy(sen, txt, n);
                                if (needs_punct) sen[n++] = '.';
                                sen[n] = '\0';
                                fc.sentences = (char**)realloc(fc.sentences, (fc.count+1)*sizeof(char*));
                                fc.sentences[fc.count++] = sen;
                            } else {
                                // replace (1-based)
                                size_t pos = (size_t)(idx - 1);
                                free(fc.sentences[pos]);
                                // Keep trailing spaces consistent: ensure sentence ends with punctuation
                                const char* txt = s->new_sentence;
                                size_t n = strlen(txt);
                                int needs_punct = (n==0 || (txt[n-1] != '.' && txt[n-1] != '!' && txt[n-1] != '?'));
                                size_t extra = needs_punct ? 1 : 0;
                                char* sen = (char*)malloc(n + extra + 1);
                                memcpy(sen, txt, n);
                                if (needs_punct) sen[n++] = '.';
                                sen[n] = '\0';
                                fc.sentences[pos] = sen;
                            }
                            char* joined = join_sentences(fc);
                            int rc = write_file_atomic(path, joined, strlen(joined));
                            free(joined);
                            if (rc == 0) { const char* ok = "OK committed"; send_u32_and_data(c, ok, (uint32_t)strlen(ok)); }
                            else { const char* e = "ERR persist"; send_u32_and_data(c, e, (uint32_t)strlen(e)); }
                        }
                        free(content);
                        free_filecontent(fc);
                    }
                    // Clear session but keep loop; release locks
                    WriteSession* done = session_take(c);
                    if (done) { free(done->fname); free(done->new_sentence); free(done); }
                    lock_release_by_fd(c);
                }
            } else {
                const char* e = "ERR"; send_u32_and_data(c, e, (uint32_t)strlen(e));
            }
            free(data);
        }
        // On disconnect, cleanup any sessions/locks held by this fd
        WriteSession* done = session_take(c);
        if (done) { free(done->fname); free(done->new_sentence); free(done); }
        lock_release_by_fd(c);
        close(c);
    }
    return NULL;
}

int main() {
    const char* nm_ip = getenv("NM_IP"); if (!nm_ip) nm_ip = "127.0.0.1";
    const char* nm_port_s = getenv("NM_PORT"); int nm_port = nm_port_s?atoi(nm_port_s):9000;
    const char* ctrl_port_s = getenv("SS_CTRL_PORT"); int ctrl_port = ctrl_port_s?atoi(ctrl_port_s):9001;
    const char* cli_port_s = getenv("SS_CLIENT_PORT"); int cli_port = cli_port_s?atoi(cli_port_s):9100;
    printf("StorageServer (C) stub starting... NM=%s:%d CTRL=%d CLI=%d\n", nm_ip, nm_port, ctrl_port, cli_port);
    ensure_dirs();
    // Listen for NM and client connections (client side not implemented yet)
    int ctrl = create_server_socket(ctrl_port, 128);
    int cli = create_server_socket(cli_port, 128);
    printf("SS ctrl fd=%d, client fd=%d\n", ctrl, cli);
    // Start accept loops BEFORE registering to avoid deadlock when NM queries LIST_FILES during registration
    pthread_t th; pthread_create(&th, NULL, ctrl_accept_loop, &ctrl); pthread_detach(th);
    pthread_t th2; pthread_create(&th2, NULL, client_accept_loop, &cli); pthread_detach(th2);
    // Register with NameServer
    int nm_sock = create_client_socket(nm_ip, nm_port);
    if (nm_sock >= 0) {
        char reg[64]; snprintf(reg, sizeof(reg), "REGISTER_SS %d %d", ctrl_port, cli_port);
        send_u32_and_data(nm_sock, reg, (uint32_t)strlen(reg));
        char* resp=NULL; uint32_t rlen=0; if (recv_u32_and_data(nm_sock, &resp, &rlen)==0) {
            printf("NM response: %.*s\n", (int)rlen, resp);
            free(resp);
        }
        close(nm_sock);
    } else {
        fprintf(stderr, "Failed to register with NM\n");
    }
    // Keep process alive
    while (1) sleep(1);
    return 0;
}
