#include "common.h"

// Global variables
int ss_id = -1;
char ss_ip[INET_ADDRSTRLEN];
int nm_port;
int client_port;
char storage_dir[MAX_PATH] = "./storage/";
pthread_mutex_t file_locks[MAX_FILES];
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int sentence_number;
    char locked_by[MAX_USERNAME];
} SentenceLock;

typedef struct {
    char filename[MAX_FILENAME];
    SentenceLock sentence_locks[100];  // Support up to 100 locked sentences per file
    int lock_count;
    char undo_content[MAX_CONTENT];
    int has_undo;
} FileLockInfo;

FileLockInfo file_lock_info[MAX_FILES];
int file_lock_count = 0;

// Replication partner info (provided by NM via OP_SS_ACK)
static int partner_set = 0;
static char partner_ip[INET_ADDRSTRLEN];
static int partner_nm_port = 0;
static int partner_client_port = 0;

static void mkdir_p_for_path(const char* fullpath) {
    // Create parent directories for fullpath
    char tmp[MAX_PATH]; strncpy(tmp, fullpath, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
    for (char* p = tmp + strlen(storage_dir); *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
}

static void replicate_send(Message* msg) {
    if (!partner_set) return;
    Message m = *msg;
    m.flags |= FLAG_REPL; // mark as replication to avoid loops
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(partner_nm_port);
    inet_pton(AF_INET, partner_ip, &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        send_message(s, &m);
        // best-effort, no wait necessary but read response to close cleanly
        receive_message(s, &m);
    }
    close(s);
}

// Function prototypes
void* handle_nm_connection(void* arg);
void* handle_client_request(void* arg);
void register_with_nm();
void handle_create_file(Message* msg);
void handle_delete_file(Message* msg);
void handle_read_file(Message* msg);
void handle_write_file(Message* msg);
void handle_stream_file(int client_sock, Message* msg);
void handle_undo_file(Message* msg);
void handle_lock_sentence(Message* msg);
void handle_unlock_sentence(Message* msg);
int get_file_lock_info(const char* filename);
void save_file_content(const char* filename, const char* content);
char* load_file_content(const char* filename);
void parse_sentences(const char* content, char sentences[][MAX_SENTENCE_LEN], int* sentence_count);
void reconstruct_content(char sentences[][MAX_SENTENCE_LEN], int sentence_count, char* output);
void load_storage_files();

// Helper: check if the last non-whitespace character in content is a sentence delimiter
static int ends_with_delimiter(const char* content) {
    if (content == NULL) return 0;
    int len = (int)strlen(content);
    int i = len - 1;
    while (i >= 0 && (content[i] == ' ' || content[i] == '\n' || content[i] == '\t' || content[i] == '\r')) {
        i--;
    }
    if (i < 0) return 0;
    return (content[i] == '.' || content[i] == '!' || content[i] == '?') ? 1 : 0;
}

int main(int argc, char* argv[]) {
    // New usage: ./storage_server [nm_ip] <nm_port> <client_port> <storage_dir>
    // Backwards compatible with old usage lacking nm_ip.
    char nm_ip_override[INET_ADDRSTRLEN] = "127.0.0.1";
    char ss_ip_override[INET_ADDRSTRLEN] = ""; // optional advertised IP override
    int argi = 1;
    if (argc < 4) {
        printf("Usage: %s [nm_ip] <nm_port> <client_port> <storage_dir> [advertise_ip]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (argc == 6) { // nm_ip + advertise_ip explicitly provided
        strncpy(nm_ip_override, argv[argi], sizeof(nm_ip_override)-1);
        nm_ip_override[sizeof(nm_ip_override)-1] = '\0';
        argi++;
    } else if (argc == 5) { // nm_ip explicitly provided (no advertise_ip)
        strncpy(nm_ip_override, argv[argi], sizeof(nm_ip_override)-1);
        nm_ip_override[sizeof(nm_ip_override)-1] = '\0';
        argi++;
    }
    nm_port = atoi(argv[argi++]);
    client_port = atoi(argv[argi++]);
    strcpy(storage_dir, argv[argi++]);
    if (argc == 6) {
        strncpy(ss_ip_override, argv[argi++], sizeof(ss_ip_override)-1);
        ss_ip_override[sizeof(ss_ip_override)-1] = '\0';
    }
    
    // Create storage directory if it doesn't exist
    mkdir(storage_dir, 0755);
    
    // Determine advertised SS IP
    // Priority: explicit arg -> env var SS_IP -> derive by UDP connect to NM -> fallback 127.0.0.1
    const char* env_ss = getenv("SS_IP");
    if (ss_ip_override[0] != '\0') {
        strncpy(ss_ip, ss_ip_override, sizeof(ss_ip)-1);
        ss_ip[sizeof(ss_ip)-1] = '\0';
    } else if (env_ss && env_ss[0] != '\0') {
        strncpy(ss_ip, env_ss, sizeof(ss_ip)-1);
        ss_ip[sizeof(ss_ip)-1] = '\0';
    } else {
        // UDP connect trick to learn the outward-facing IP used to reach NM
        int us = socket(AF_INET, SOCK_DGRAM, 0);
        if (us >= 0) {
            struct sockaddr_in probe; memset(&probe, 0, sizeof(probe));
            probe.sin_family = AF_INET; probe.sin_port = htons(PORT_NM);
            inet_pton(AF_INET, nm_ip_override, &probe.sin_addr);
            if (connect(us, (struct sockaddr*)&probe, sizeof(probe)) == 0) {
                struct sockaddr_in local; socklen_t llen = sizeof(local);
                if (getsockname(us, (struct sockaddr*)&local, &llen) == 0) {
                    inet_ntop(AF_INET, &local.sin_addr, ss_ip, sizeof(ss_ip));
                } else {
                    strcpy(ss_ip, "127.0.0.1");
                }
            } else {
                strcpy(ss_ip, "127.0.0.1");
            }
            close(us);
        } else {
            strcpy(ss_ip, "127.0.0.1");
        }
    }
    
    printf("=== LangOS Storage Server ===\n");
    log_message("SS", "INFO", "Starting Storage Server on %s, ports NM:%d Client:%d", ss_ip, nm_port, client_port);
    
    // Initialize file locks
    for (int i = 0; i < MAX_FILES; i++) {
        pthread_mutex_init(&file_locks[i], NULL);
        file_lock_info[i].lock_count = 0;
        file_lock_info[i].has_undo = 0;
    }

    // Populate file_lock_info from existing files in storage_dir so locks and undo work after restarts
    load_storage_files();
    
    // Register with Name Server
    // Register with Name Server using override IP
    int sock = create_socket();
    if (sock < 0) {
        log_message("SS", "ERROR", "Failed to create socket for NM registration");
        exit(EXIT_FAILURE);
    }
    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(PORT_NM);
    if (inet_pton(AF_INET, nm_ip_override, &nm_addr.sin_addr) != 1) {
        log_message("SS", "ERROR", "Invalid NM IP: %s", nm_ip_override);
        exit(EXIT_FAILURE);
    }
    if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Failed to connect to Name Server");
        log_message("SS", "ERROR", "Failed to connect to Name Server at %s:%d", nm_ip_override, PORT_NM);
        exit(EXIT_FAILURE);
    }
    Message regmsg; memset(&regmsg, 0, sizeof(regmsg));
    regmsg.op_code = OP_REGISTER_SS;
    sprintf(regmsg.data, "%s %d %d", ss_ip, nm_port, client_port);
    send_message(sock, &regmsg);
    receive_message(sock, &regmsg);
    if (regmsg.error_code == ERR_SUCCESS) {
        ss_id = atoi(regmsg.data);
        log_message("SS", "INFO", "Registered with NM %s:%d, assigned ID: %d", nm_ip_override, PORT_NM, ss_id);
    } else {
        log_message("SS", "ERROR", "NM registration failed: %s", regmsg.error_msg);
        exit(EXIT_FAILURE);
    }
    close(sock);
    
    // Start client listener thread
    pthread_t nm_thread, client_thread;
    
    int* nm_port_ptr = malloc(sizeof(int));
    *nm_port_ptr = nm_port;
    pthread_create(&nm_thread, NULL, handle_nm_connection, nm_port_ptr);
    pthread_detach(nm_thread);
    
    int* client_port_ptr = malloc(sizeof(int));
    *client_port_ptr = client_port;
    pthread_create(&client_thread, NULL, handle_client_request, client_port_ptr);
    pthread_detach(client_thread);
    
    // Keep running
    while (1) {
        sleep(1);
    }
    
    return 0;
}

// Load existing files from storage directory into file_lock_info
void load_storage_files() {
    DIR* d = opendir(storage_dir);
    if (!d) return;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        // skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        // Skip metadata files
        size_t len = strlen(entry->d_name);
        if (len > 5 && strcmp(entry->d_name + len - 5, ".meta") == 0) continue;

        // Only add regular files (ignore directories)
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s%s", storage_dir, entry->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        // Add to file_lock_info if space
        pthread_mutex_lock(&global_lock);
        if (file_lock_count < MAX_FILES) {
            strncpy(file_lock_info[file_lock_count].filename, entry->d_name, MAX_FILENAME - 1);
            file_lock_info[file_lock_count].filename[MAX_FILENAME - 1] = '\0';
            file_lock_info[file_lock_count].lock_count = 0;
            file_lock_info[file_lock_count].has_undo = 0;
            file_lock_count++;
            log_message("SS", "INFO", "Discovered file on startup: %s", entry->d_name);
        }
        pthread_mutex_unlock(&global_lock);
    }

    closedir(d);
}

// Legacy register_with_nm retained for compatibility (unused in new main)
void register_with_nm() { /* Deprecated path now handled inline in main */ }

void* handle_nm_connection(void* arg) {
    int port = *(int*)arg;
    free(arg);
    
    int server_sock = create_socket();
    if (server_sock < 0) {
        log_message("SS", "ERROR", "Failed to create NM listener socket");
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("NM bind failed");
        return NULL;
    }
    
    if (listen(server_sock, 10) < 0) {
        perror("NM listen failed");
        return NULL;
    }
    
    log_message("SS", "INFO", "Listening for NM connections on port %d", port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }
        
        Message msg;
        if (receive_message(client_sock, &msg) > 0) {
            switch (msg.op_code) {
                case OP_CREATE:
                    handle_create_file(&msg);
                    break;
                case OP_DELETE:
                    handle_delete_file(&msg);
                    break;
                case OP_READ:
                    handle_read_file(&msg);
                    break;
                case OP_CREATEFOLDER: {
                    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s%s", storage_dir, msg.filename);
                    mkdir(path, 0755); // best-effort single level
                    mkdir_p_for_path(path);
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "Folder created");
                    // Replicate folder creation (best-effort) to partner
                    if (!(msg.flags & FLAG_REPL)) {
                        Message rm = msg; rm.op_code = OP_REPL_CREATEFOLDER; replicate_send(&rm);
                    }
                    break;
                }
                case OP_MOVE: {
                    // MOVE from msg.filename to msg.data
                    char src[MAX_PATH]; snprintf(src, sizeof(src), "%s%s", storage_dir, msg.filename);
                    char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s%s", storage_dir, msg.data);
                    char newpath[MAX_FILENAME]; strncpy(newpath, msg.data, sizeof(newpath)-1); newpath[sizeof(newpath)-1] = '\0';
                    mkdir_p_for_path(dst);
                    if (rename(src, dst) == 0) {
                        // Move .meta too (best-effort)
                        char srcm[MAX_PATH]; snprintf(srcm, sizeof(srcm), "%s%s.meta", storage_dir, msg.filename);
                        char dstm[MAX_PATH]; snprintf(dstm, sizeof(dstm), "%s%s.meta", storage_dir, msg.data);
                        mkdir_p_for_path(dstm);
                        rename(srcm, dstm);
                        // Prepare replication BEFORE overwriting msg.data (need new path)
                        if (!(msg.flags & FLAG_REPL)) {
                            Message rm = msg; rm.op_code = OP_REPL_MOVE; strncpy(rm.data, newpath, sizeof(rm.data)-1); rm.data[sizeof(rm.data)-1]='\0'; replicate_send(&rm);
                        }
                        msg.error_code = ERR_SUCCESS; // Preserve new path in response for correctness
                        strncpy(msg.data, newpath, sizeof(msg.data)-1); msg.data[sizeof(msg.data)-1] = '\0';
                        strncpy(msg.error_msg, "Move successful", sizeof(msg.error_msg)-1);
                    } else { msg.error_code = ERR_SERVER_ERROR; strcpy(msg.error_msg, "Move failed"); }
                    break;
                }
                case OP_SS_ACK: {
                    // data: partner_ip partner_nm_port partner_client_port
                    sscanf(msg.data, "%15s %d %d", partner_ip, &partner_nm_port, &partner_client_port);
                    partner_set = 1;
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "ACK");
                    break;
                }
                case OP_REPL_CREATE:
                    handle_create_file(&msg); // treat as normal without further replication
                    break;
                case OP_REPL_DELETE:
                    handle_delete_file(&msg);
                    break;
                case OP_REPL_MOVE: {
                    // reuse OP_MOVE logic
                    char src[MAX_PATH]; snprintf(src, sizeof(src), "%s%s", storage_dir, msg.filename);
                    char dst[MAX_PATH]; snprintf(dst, sizeof(dst), "%s%s", storage_dir, msg.data);
                    mkdir_p_for_path(dst);
                    if (rename(src, dst) == 0) {
                        // Move meta file too
                        char srcm[MAX_PATH]; snprintf(srcm, sizeof(srcm), "%s%s.meta", storage_dir, msg.filename);
                        char dstm[MAX_PATH]; snprintf(dstm, sizeof(dstm), "%s%s.meta", storage_dir, msg.data);
                        mkdir_p_for_path(dstm);
                        rename(srcm, dstm);
                        msg.error_code = ERR_SUCCESS; // Keep destination path in data for potential debugging
                        strncpy(msg.error_msg, "Move successful", sizeof(msg.error_msg)-1);
                    } else { msg.error_code = ERR_SERVER_ERROR; strcpy(msg.error_msg, "Move failed"); }
                    break;
                }
                case OP_REPL_CREATEFOLDER: {
                    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s%s", storage_dir, msg.filename);
                    mkdir(path, 0755);
                    mkdir_p_for_path(path);
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "Folder created");
                    break;
                }
                case OP_REPL_WRITE: {
                    // Overwrite content with replicated data
                    save_file_content(msg.filename, msg.data);
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "Replicated");
                    break;
                }
                default:
                    msg.error_code = ERR_INVALID_COMMAND;
                    strcpy(msg.error_msg, "Invalid command from NM");
                    break;
            }
            send_message(client_sock, &msg);
        }
        
        close(client_sock);
    }
    
    close(server_sock);
    return NULL;
}

void* handle_client_request(void* arg) {
    int port = *(int*)arg;
    free(arg);
    
    int server_sock = create_socket();
    if (server_sock < 0) {
        log_message("SS", "ERROR", "Failed to create client listener socket");
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Client bind failed");
        return NULL;
    }
    
    if (listen(server_sock, 50) < 0) {
        perror("Client listen failed");
        return NULL;
    }
    
    log_message("SS", "INFO", "Listening for client connections on port %d", port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }
        
        Message msg;
        if (receive_message(client_sock, &msg) > 0) {
            log_request("SS", "client", client_sock, msg.username, "Client operation");
            
            switch (msg.op_code) {
                case OP_READ:
                    handle_read_file(&msg);
                    send_message(client_sock, &msg);
                    break;
                case OP_WRITE:
                    handle_write_file(&msg);
                    send_message(client_sock, &msg);
                    break;
                case OP_STREAM:
                    handle_stream_file(client_sock, &msg);
                    break;
                case OP_UNDO:
                    handle_undo_file(&msg);
                    send_message(client_sock, &msg);
                    break;
                case OP_CHECKPOINT: {
                    // data: checkpoint_tag
                    char* content = load_file_content(msg.filename);
                    if (!content) { msg.error_code = ERR_FILE_NOT_FOUND; strcpy(msg.error_msg, "File not found"); send_message(client_sock, &msg); break; }
                    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s.checkpoints/%s/%s", storage_dir, msg.filename, msg.data);
                    mkdir_p_for_path(path);
                    FILE* fp = fopen(path, "w"); if (!fp) { free(content); msg.error_code=ERR_SERVER_ERROR; strcpy(msg.error_msg, "Failed to create checkpoint"); send_message(client_sock,&msg); break; }
                    fprintf(fp, "%s", content); fclose(fp); free(content);
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "Checkpoint created"); send_message(client_sock, &msg);
                    break;
                }
                case OP_VIEWCHECKPOINT: {
                    // data: checkpoint_tag
                    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s.checkpoints/%s/%s", storage_dir, msg.filename, msg.data);
                    FILE* fp = fopen(path, "r"); if (!fp) { msg.error_code=ERR_FILE_NOT_FOUND; strcpy(msg.error_msg, "Checkpoint not found"); send_message(client_sock,&msg); break; }
                    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                    char* buf = malloc(sz+1); if (!buf){ fclose(fp); msg.error_code=ERR_SERVER_ERROR; strcpy(msg.error_msg, "OOM"); send_message(client_sock,&msg); break; }
                    fread(buf,1,sz,fp); buf[sz]='\0'; fclose(fp);
                    strncpy(msg.data, buf, sizeof(msg.data)-1); free(buf);
                    msg.error_code = ERR_SUCCESS; send_message(client_sock,&msg);
                    break;
                }
                case OP_REVERT: {
                    // data: checkpoint_tag
                    char path[MAX_PATH]; snprintf(path, sizeof(path), "%s.checkpoints/%s/%s", storage_dir, msg.filename, msg.data);
                    FILE* fp = fopen(path, "r"); if (!fp) { msg.error_code=ERR_FILE_NOT_FOUND; strcpy(msg.error_msg, "Checkpoint not found"); send_message(client_sock,&msg); break; }
                    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                    char* buf = malloc(sz+1); if (!buf){ fclose(fp); msg.error_code=ERR_SERVER_ERROR; strcpy(msg.error_msg, "OOM"); send_message(client_sock,&msg); break; }
                    fread(buf,1,sz,fp); buf[sz]='\0'; fclose(fp);
                    save_file_content(msg.filename, buf);
                    // Replicate revert as write
                    Message rm = msg; rm.op_code = OP_REPL_WRITE; strncpy(rm.data, buf, sizeof(rm.data)-1); if (!(rm.flags & FLAG_REPL)) replicate_send(&rm);
                    free(buf);
                    msg.error_code = ERR_SUCCESS; strcpy(msg.data, "Reverted"); send_message(client_sock,&msg);
                    break;
                }
                case OP_LISTCHECKPOINTS: {
                    char dirpath[MAX_PATH]; snprintf(dirpath, sizeof(dirpath), "%s.checkpoints/%s", storage_dir, msg.filename);
                    DIR* d = opendir(dirpath);
                    if (!d) { msg.error_code = ERR_SUCCESS; msg.data[0]='\0'; send_message(client_sock,&msg); break; }
                    struct dirent* ent; msg.data[0]='\0';
                    while ((ent = readdir(d)) != NULL) {
                        if (strcmp(ent->d_name, ".")==0 || strcmp(ent->d_name, "..")==0) continue;
                        strcat(msg.data, "--> "); strcat(msg.data, ent->d_name); strcat(msg.data, "\n");
                    }
                    closedir(d);
                    msg.error_code = ERR_SUCCESS; send_message(client_sock, &msg);
                    break;
                }
                case OP_LOCK_SENTENCE:
                    handle_lock_sentence(&msg);
                    send_message(client_sock, &msg);
                    break;
                case OP_UNLOCK_SENTENCE:
                    handle_unlock_sentence(&msg);
                    send_message(client_sock, &msg);
                    break;
                default:
                    msg.error_code = ERR_INVALID_COMMAND;
                    strcpy(msg.error_msg, "Invalid command");
                    send_message(client_sock, &msg);
                    break;
            }
        }
        
        close(client_sock);
    }
    
    close(server_sock);
    return NULL;
}

void handle_create_file(Message* msg) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg->filename);
    
    FILE* fp = fopen(filepath, "w");
    if (fp == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Failed to create file");
        log_message("SS", "ERROR", "Failed to create file: %s", msg->filename);
        return;
    }
    
    fclose(fp);
    
    // Create metadata file
    char meta_path[MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s%s.meta", storage_dir, msg->filename);
    fp = fopen(meta_path, "w");
    if (fp) {
        fprintf(fp, "created:%ld\n", time(NULL));
        fclose(fp);
    }
    
    pthread_mutex_lock(&global_lock);
    strcpy(file_lock_info[file_lock_count].filename, msg->filename);
    file_lock_info[file_lock_count].lock_count = 0;
    file_lock_info[file_lock_count].has_undo = 0;
    file_lock_count++;
    pthread_mutex_unlock(&global_lock);
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File created successfully");
    log_message("SS", "INFO", "File created: %s", msg->filename);

    // Replicate creation to partner
    if (!(msg->flags & FLAG_REPL)) {
        Message rm = *msg; rm.op_code = OP_REPL_CREATE; replicate_send(&rm);
    }
}

void handle_delete_file(Message* msg) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, msg->filename);
    
    if (unlink(filepath) != 0) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Failed to delete file");
        log_message("SS", "ERROR", "Failed to delete file: %s", msg->filename);
        return;
    }
    
    // Delete metadata
    char meta_path[MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s%s.meta", storage_dir, msg->filename);
    unlink(meta_path);
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File deleted successfully");
    log_message("SS", "INFO", "File deleted: %s", msg->filename);

    // Replicate deletion to partner
    if (!(msg->flags & FLAG_REPL)) {
        Message rm = *msg; rm.op_code = OP_REPL_DELETE; replicate_send(&rm);
    }
}

void handle_read_file(Message* msg) {
    char* content = load_file_content(msg->filename);
    
    if (content == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "Failed to read file");
        return;
    }
    
    strncpy(msg->data, content, MAX_CONTENT - 1);
    msg->data[MAX_CONTENT - 1] = '\0';
    msg->error_code = ERR_SUCCESS;
    
    free(content);
    log_message("SS", "INFO", "File read: %s by %s", msg->filename, msg->username);
}

void handle_write_file(Message* msg) {
    log_message("SS", "INFO", "WRITE request for %s sentence %d by %s", 
                msg->filename, msg->sentence_number, msg->username);
    
    int lock_index = get_file_lock_info(msg->filename);
    
    if (lock_index < 0) {
        log_message("SS", "ERROR", "File lock info not found for %s", msg->filename);
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        return;
    }
    
    pthread_mutex_lock(&file_locks[lock_index]);
    
    FileLockInfo* lock_info = &file_lock_info[lock_index];
    
    // Load current content
    char* content = load_file_content(msg->filename);
    int last_has_delim = ends_with_delimiter(content);
    if (content == NULL) {
        log_message("SS", "INFO", "Empty file, creating new content");
        // File doesn't exist or can't be read - create empty content
        content = (char*)malloc(1);
        if (content) {
            content[0] = '\0';
        } else {
            msg->error_code = ERR_SERVER_ERROR;
            strcpy(msg->error_msg, "Memory allocation failed");
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }
    }

    log_message("SS", "INFO", "Loaded content, length: %zu", strlen(content));

    // Save for undo
    strncpy(lock_info->undo_content, content, MAX_CONTENT - 1);
    lock_info->undo_content[MAX_CONTENT - 1] = '\0';
    lock_info->has_undo = 1;
    
    // Parse into sentences (split by sentence delimiters)
    char (*sentences)[MAX_SENTENCE_LEN] = calloc(1000, sizeof(char[MAX_SENTENCE_LEN]));
    if (sentences == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Memory allocation failed");
        free(content);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    int sentence_count = 0;
    parse_sentences(content, sentences, &sentence_count);

    free(content);
    content = NULL;

    log_message("SS", "INFO", "Parsed %d sentences", sentence_count);

    // Validate sentence index. Normally allow append (== sentence_count),
    // BUT disallow append if the current content doesn't end with a delimiter.
    if (msg->sentence_number < 0 || msg->sentence_number > sentence_count) {
        msg->error_code = ERR_INVALID_INDEX;
        sprintf(msg->error_msg, "Sentence index out of range (0-%d allowed)", sentence_count);
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    // If writing to a new sentence (== sentence_count), only allow when
    // the existing content properly ends with a sentence delimiter.
    if (msg->sentence_number == sentence_count) {
        if (sentence_count > 0 && !last_has_delim) {
            msg->error_code = ERR_INVALID_INDEX;
            sprintf(msg->error_msg, "Sentence index out of range (0-%d allowed). Terminate previous sentence to add a new one.", sentence_count - 1);
            free(sentences);
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }
        sentence_count++;
        sentences[msg->sentence_number][0] = '\0';
    }

    // Verify the sentence is locked by this user
    int has_lock = 0;
    for (int i = 0; i < lock_info->lock_count; i++) {
        if (lock_info->sentence_locks[i].sentence_number == msg->sentence_number &&
            strcmp(lock_info->sentence_locks[i].locked_by, msg->username) == 0) {
            has_lock = 1;
            break;
        }
    }

    if (!has_lock) {
        msg->error_code = ERR_SENTENCE_LOCKED;
        strcpy(msg->error_msg, "Sentence must be locked before writing");
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        log_message("SS", "ERROR", "Write attempt without lock by %s on sentence %d",
                    msg->username, msg->sentence_number);
        return;
    }

    log_message("SS", "INFO", "Processing write data: %s", msg->data);

    // Build token list from the current sentence words (each existing word is one token)
    // Insert each user line as a single phrase token to keep multi-word inserts contiguous
    char (*tokens)[MAX_SENTENCE_LEN] = calloc(500, sizeof(char[MAX_SENTENCE_LEN]));
    if (tokens == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Memory allocation failed");
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    int token_count = 0;
    if (sentences[msg->sentence_number][0] != '\0') {
        // Split existing sentence into words as separate tokens
        char* sentence_copy = strdup(sentences[msg->sentence_number]);
        if (sentence_copy == NULL) {
            msg->error_code = ERR_SERVER_ERROR;
            strcpy(msg->error_msg, "Memory allocation failed");
            free(tokens);
            free(sentences);
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }

        char* saveptr2 = NULL;
        char* w = strtok_r(sentence_copy, " ", &saveptr2);
        while (w != NULL && token_count < 500) {
            strncpy(tokens[token_count], w, MAX_SENTENCE_LEN - 1);
            tokens[token_count][MAX_SENTENCE_LEN - 1] = '\0';
            token_count++;
            w = strtok_r(NULL, " ", &saveptr2);
        }
        free(sentence_copy);
    }

    // Process each user-provided line (format: <word_index> <content>)
    char* data_copy = strdup(msg->data);
    if (data_copy == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Memory allocation failed");
        free(tokens);
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    char* saveptr1 = NULL;
    char* line = strtok_r(data_copy, "\n", &saveptr1);
    while (line != NULL) {
        int word_index;
        if (sscanf(line, "%d", &word_index) != 1) {
            line = strtok_r(NULL, "\n", &saveptr1);
            continue;
        }

        // Find the start of content after number and spaces
        char* content_start = line;
        while (*content_start && (*content_start == ' ' || (*content_start >= '0' && *content_start <= '9'))) {
            content_start++;
        }

        if (!*content_start) { // Empty content line; skip
            line = strtok_r(NULL, "\n", &saveptr1);
            continue;
        }

        // Validate index against current token count (phrase-aware)
        if (word_index < 1 || word_index > token_count + 1) {
            msg->error_code = ERR_INVALID_INDEX;
            sprintf(msg->error_msg, "Word index out of range (1-%d allowed)", token_count + 1);
            free(data_copy);
            free(tokens);
            free(sentences);
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }

        if (token_count >= 500) {
            msg->error_code = ERR_SERVER_ERROR;
            strcpy(msg->error_msg, "Too many tokens in sentence");
            free(data_copy);
            free(tokens);
            free(sentences);
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }

        // Insert the entire content as a single phrase token at position (word_index - 1)
        int insert_pos = word_index - 1; // 0-based

        for (int i = token_count - 1; i >= insert_pos; --i) {
            strcpy(tokens[i + 1], tokens[i]);
        }

        // Trim and assign content
        char phrase[MAX_SENTENCE_LEN];
        strncpy(phrase, content_start, sizeof(phrase) - 1);
        phrase[sizeof(phrase) - 1] = '\0';
        trim_whitespace(phrase);
        strncpy(tokens[insert_pos], phrase, MAX_SENTENCE_LEN - 1);
        tokens[insert_pos][MAX_SENTENCE_LEN - 1] = '\0';

        token_count++;
        line = strtok_r(NULL, "\n", &saveptr1);
    }

    free(data_copy);

    // Rebuild the updated sentence by joining tokens with spaces
    sentences[msg->sentence_number][0] = '\0';
    for (int i = 0; i < token_count; i++) {
        size_t current_len = strlen(sentences[msg->sentence_number]);
        size_t token_len = strlen(tokens[i]);
        if (current_len + token_len + 2 < MAX_SENTENCE_LEN) {
            if (i > 0) strcat(sentences[msg->sentence_number], " ");
            strcat(sentences[msg->sentence_number], tokens[i]);
        }
    }

    free(tokens);

    // Reconstruct file content from sentences[] and persist
    char* new_content = malloc(MAX_CONTENT);
    if (new_content == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Memory allocation failed");
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    new_content[0] = '\0';
    reconstruct_content(sentences, sentence_count, new_content);

    // Save file
    save_file_content(msg->filename, new_content);

    log_message("SS", "INFO", "Saved new content, length: %zu", strlen(new_content));

    // Replicate write to partner (best-effort)
    if (!(msg->flags & FLAG_REPL)) {
        Message rm = *msg; rm.op_code = OP_REPL_WRITE; rm.data[0] = '\0';
        // Ensure data carries content to write
        strncpy(rm.data, new_content, sizeof(rm.data)-1);
        replicate_send(&rm);
    }

    free(new_content);
    free(sentences);
    
    // Note: Sentence remains locked until client explicitly unlocks via ETIRW
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Write successful");
    
    pthread_mutex_unlock(&file_locks[lock_index]);
    
    log_message("SS", "INFO", "Write completed successfully for %s", msg->filename);
}

void handle_stream_file(int client_sock, Message* msg) {
    char* content = load_file_content(msg->filename);
    
    if (content == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "Failed to read file");
        send_message(client_sock, msg);
        return;
    }
    
    // Send success first
    msg->error_code = ERR_SUCCESS;
    msg->data[0] = '\0';
    send_message(client_sock, msg);
    
    // Stream word by word
    char* word = strtok(content, " \n\t");
    while (word != NULL) {
        memset(msg, 0, sizeof(Message));
        strcpy(msg->data, word);
        send_message(client_sock, msg);
        usleep(100000);  // 0.1 second delay
        word = strtok(NULL, " \n\t");
    }
    
    // Send stop signal
    memset(msg, 0, sizeof(Message));
    strcpy(msg->data, "STOP");
    send_message(client_sock, msg);
    
    free(content);
    log_message("SS", "INFO", "File streamed: %s to %s", msg->filename, msg->username);
}

void handle_undo_file(Message* msg) {
    int lock_index = get_file_lock_info(msg->filename);
    
    if (lock_index < 0) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        return;
    }
    
    pthread_mutex_lock(&file_locks[lock_index]);
    
    FileLockInfo* lock_info = &file_lock_info[lock_index];
    
    if (!lock_info->has_undo) {
        msg->error_code = ERR_NO_UNDO;
        strcpy(msg->error_msg, "No undo history available");
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }
    
    // Restore from undo
    save_file_content(msg->filename, lock_info->undo_content);
    lock_info->has_undo = 0;
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Undo successful");
    
    pthread_mutex_unlock(&file_locks[lock_index]);
    
    log_message("SS", "INFO", "File undo: %s by %s", msg->filename, msg->username);
}

void handle_lock_sentence(Message* msg) {
    log_message("SS", "INFO", "LOCK request for %s sentence %d by %s", 
                msg->filename, msg->sentence_number, msg->username);
    
    int lock_index = get_file_lock_info(msg->filename);
    
    if (lock_index < 0) {
        log_message("SS", "ERROR", "File lock info not found for %s", msg->filename);
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        return;
    }
    
    pthread_mutex_lock(&file_locks[lock_index]);
    
    // Validate sentence index: must be in [0, current_sentence_count].
    // Allow locking a new sentence by permitting == current_sentence_count,
    // BUT only if existing content ends with a delimiter.
    int current_sentence_count = 0;
    int last_has_delim = 0;
    {
        char* content = load_file_content(msg->filename);
        // Parse existing content to determine current number of sentences
        char (*sentences)[MAX_SENTENCE_LEN] = calloc(100, sizeof(char[MAX_SENTENCE_LEN]));
        if (sentences != NULL) {
            parse_sentences(content ? content : "", sentences, &current_sentence_count);
            free(sentences);
        }
        last_has_delim = ends_with_delimiter(content);
        if (content) free(content);
    }

    if (msg->sentence_number < 0 || msg->sentence_number > current_sentence_count) {
        msg->error_code = ERR_INVALID_INDEX;
        sprintf(msg->error_msg, "Sentence index out of range (0-%d allowed)", current_sentence_count);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }

    if (msg->sentence_number == current_sentence_count) {
        if (current_sentence_count > 0 && !last_has_delim) {
            msg->error_code = ERR_INVALID_INDEX;
            sprintf(msg->error_msg, "Sentence index out of range (0-%d allowed). Terminate previous sentence to add a new one.", current_sentence_count - 1);
            pthread_mutex_unlock(&file_locks[lock_index]);
            return;
        }
    }

    FileLockInfo* lock_info = &file_lock_info[lock_index];
    
    // Check if sentence is already locked by another user
    for (int i = 0; i < lock_info->lock_count; i++) {
        if (lock_info->sentence_locks[i].sentence_number == msg->sentence_number) {
            if (strcmp(lock_info->sentence_locks[i].locked_by, msg->username) != 0) {
                log_message("SS", "INFO", "Sentence %d already locked by %s", 
                            msg->sentence_number, lock_info->sentence_locks[i].locked_by);
                msg->error_code = ERR_SENTENCE_LOCKED;
                sprintf(msg->error_msg, "Sentence %d is locked by %s", 
                        msg->sentence_number, lock_info->sentence_locks[i].locked_by);
                pthread_mutex_unlock(&file_locks[lock_index]);
                return;
            } else {
                // User already owns this lock, just return success
                msg->error_code = ERR_SUCCESS;
                strcpy(msg->data, "Sentence already locked by you");
                pthread_mutex_unlock(&file_locks[lock_index]);
                return;
            }
        }
    }
    
    // Add new lock
    if (lock_info->lock_count >= 100) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Too many locks on this file");
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }
    
    lock_info->sentence_locks[lock_info->lock_count].sentence_number = msg->sentence_number;
    strcpy(lock_info->sentence_locks[lock_info->lock_count].locked_by, msg->username);
    lock_info->lock_count++;
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Sentence locked");
    
    pthread_mutex_unlock(&file_locks[lock_index]);
    
    log_message("SS", "INFO", "Sentence %d locked by %s (total locks: %d)", 
                msg->sentence_number, msg->username, lock_info->lock_count);
}

void handle_unlock_sentence(Message* msg) {
    log_message("SS", "INFO", "UNLOCK request for %s sentence %d by %s", 
                msg->filename, msg->sentence_number, msg->username);
    
    int lock_index = get_file_lock_info(msg->filename);
    
    if (lock_index < 0) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        return;
    }
    
    pthread_mutex_lock(&file_locks[lock_index]);
    
    FileLockInfo* lock_info = &file_lock_info[lock_index];
    
    // Find and remove the lock
    int found = 0;
    for (int i = 0; i < lock_info->lock_count; i++) {
        if (lock_info->sentence_locks[i].sentence_number == msg->sentence_number) {
            if (strcmp(lock_info->sentence_locks[i].locked_by, msg->username) == 0) {
                // Remove this lock by shifting remaining locks
                for (int j = i; j < lock_info->lock_count - 1; j++) {
                    lock_info->sentence_locks[j] = lock_info->sentence_locks[j + 1];
                }
                lock_info->lock_count--;
                found = 1;
                
                msg->error_code = ERR_SUCCESS;
                strcpy(msg->data, "Sentence unlocked");
                log_message("SS", "INFO", "Sentence %d unlocked by %s (remaining locks: %d)", 
                            msg->sentence_number, msg->username, lock_info->lock_count);
                break;
            } else {
                msg->error_code = ERR_ACCESS_DENIED;
                strcpy(msg->error_msg, "You don't own this lock");
                found = 1;
                break;
            }
        }
    }
    
    if (!found) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->error_msg, "Sentence is not locked");
    }
    
    pthread_mutex_unlock(&file_locks[lock_index]);
}

int get_file_lock_info(const char* filename) {
    // Fast path: existing entry
    for (int i = 0; i < file_lock_count; i++) {
        if (strcmp(file_lock_info[i].filename, filename) == 0) {
            return i;
        }
    }

    // Not found. If the file exists on disk (e.g., after SS restart), lazily create lock info.
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    if (access(filepath, F_OK) == 0) {
        pthread_mutex_lock(&global_lock);
        // Re-check under lock to avoid races
        for (int i = 0; i < file_lock_count; i++) {
            if (strcmp(file_lock_info[i].filename, filename) == 0) {
                pthread_mutex_unlock(&global_lock);
                return i;
            }
        }

        if (file_lock_count < MAX_FILES) {
            int idx = file_lock_count;
            strncpy(file_lock_info[idx].filename, filename, MAX_FILENAME - 1);
            file_lock_info[idx].filename[MAX_FILENAME - 1] = '\0';
            file_lock_info[idx].lock_count = 0;
            file_lock_info[idx].has_undo = 0;
            file_lock_count++;
            pthread_mutex_unlock(&global_lock);
            return idx;
        }
        pthread_mutex_unlock(&global_lock);
    }

    // File truly unknown
    return -1;
}

void save_file_content(const char* filename, const char* content) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    mkdir_p_for_path(filepath);
    
    FILE* fp = fopen(filepath, "w");
    if (fp) {
        fprintf(fp, "%s", content);
        fclose(fp);
    }
}

char* load_file_content(const char* filename) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
    FILE* fp = fopen(filepath, "r");
    if (fp == NULL) {
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    if (content) {
        fread(content, 1, size, fp);
        content[size] = '\0';
    }
    
    fclose(fp);
    return content;
}

void parse_sentences(const char* content, char sentences[][MAX_SENTENCE_LEN], int* sentence_count) {
    *sentence_count = 0;
    
    if (content == NULL || strlen(content) == 0) {
        return;
    }
    
    char current[MAX_SENTENCE_LEN] = "";
    int pos = 0;
    
    for (int i = 0; content[i] != '\0' && *sentence_count < 1000; i++) {
        if (pos < MAX_SENTENCE_LEN - 1) {
            current[pos++] = content[i];
        }
        
        if (content[i] == '.' || content[i] == '!' || content[i] == '?') {
            current[pos] = '\0';
            
            // Trim leading/trailing spaces
            int start = 0;
            while (start < pos && (current[start] == ' ' || current[start] == '\n' || current[start] == '\t')) {
                start++;
            }
            
            int end = pos - 1;
            while (end > start && (current[end] == ' ' || current[end] == '\n' || current[end] == '\t')) {
                end--;
            }
            
            if (start <= end) {
                int len = end - start + 1;
                if (len > 0 && len < MAX_SENTENCE_LEN) {
                    strncpy(sentences[*sentence_count], &current[start], len + 1);
                    sentences[*sentence_count][len + 1] = '\0';
                    (*sentence_count)++;
                }
            }
            
            pos = 0;
            current[0] = '\0';
        }
    }
    
    // Handle remaining content (sentence without delimiter)
    if (pos > 0 && *sentence_count < 1000) {
        current[pos] = '\0';
        int start = 0;
        while (start < pos && (current[start] == ' ' || current[start] == '\n' || current[start] == '\t')) {
            start++;
        }
        
        int end = pos - 1;
        while (end > start && (current[end] == ' ' || current[end] == '\n' || current[end] == '\t')) {
            end--;
        }
        
        if (start <= end) {
            int len = end - start + 1;
            if (len > 0 && len < MAX_SENTENCE_LEN) {
                strncpy(sentences[*sentence_count], &current[start], len + 1);
                sentences[*sentence_count][len + 1] = '\0';
                (*sentence_count)++;
            }
        }
    }
}

void reconstruct_content(char sentences[][MAX_SENTENCE_LEN], int sentence_count, char* output) {
    output[0] = '\0';
    
    for (int i = 0; i < sentence_count; i++) {
        strcat(output, sentences[i]);
        if (i < sentence_count - 1) {
            strcat(output, " ");
        }
    }
}
