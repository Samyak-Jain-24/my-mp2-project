#include "common.h"
#include <sys/time.h>
#include <signal.h>
#include <ctype.h>

// Graceful shutdown control
static volatile sig_atomic_t nm_running = 1;
// Forward declaration of listening socket defined later
extern int nm_socket;

static void handle_sigint(int sig) {
    (void)sig;
    nm_running = 0;
    log_message("NM", "INFO", "SIGINT received, shutting down...");
    // Close listening socket to break accept()
    if (nm_socket > 0) close(nm_socket);
}

// Global variables
StorageServerInfo storage_servers[MAX_SS];
ClientInfo clients[MAX_CLIENTS];
FileMetadata files[MAX_FILES];
TrieNode* file_trie_root;
int ss_count = 0;
int client_count = 0;
int file_count = 0;
pthread_mutex_t nm_lock = PTHREAD_MUTEX_INITIALIZER;
int nm_socket;

// Cache for recent searches
typedef struct {
    char filename[MAX_FILENAME];
    FileMetadata* file_info;
    time_t timestamp;
} CacheEntry;

CacheEntry search_cache[100];
int cache_size = 0;

// Function prototypes
void* handle_ss_connection(void* arg);
void* handle_client_connection(void* arg);
void register_storage_server(int socket_fd, Message* msg);
void register_client(int socket_fd, Message* msg);
int find_ss_for_file(const char* filename);
void handle_view_command(int client_sock, Message* msg);
void handle_create_command(int client_sock, Message* msg);
void handle_delete_command(int client_sock, Message* msg);
void handle_info_command(int client_sock, Message* msg);
void handle_list_command(int client_sock, Message* msg);
void handle_addaccess_command(int client_sock, Message* msg);
void handle_remaccess_command(int client_sock, Message* msg);
void handle_exec_command(int client_sock, Message* msg);
void handle_read_stream_undo_command(int client_sock, Message* msg);
void handle_write_command(int client_sock, Message* msg);
// Bonus handlers
void handle_createfolder_command(int client_sock, Message* msg);
void handle_viewfolder_command(int client_sock, Message* msg);
void handle_move_command(int client_sock, Message* msg);
void handle_reqaccess_command(int client_sock, Message* msg);
void handle_viewrequests_command(int client_sock, Message* msg);
void handle_approve_command(int client_sock, Message* msg);
void handle_deny_command(int client_sock, Message* msg);
void handle_recents_command(int client_sock, Message* msg);
FileMetadata* search_file_cached(const char* filename);
void update_cache(const char* filename, FileMetadata* file_info);
void load_persistent_data();
void save_persistent_data();

// Helpers to validate SS state and purge stale metadata
static int ss_file_exists(FileMetadata* file);
static void purge_file_metadata(const char* filename);
// Forward declarations for heartbeat & sync threads
void* storage_server_heartbeat_loop(void* arg);
void* sync_returned_primary(void* arg);

int main() {
    // Register SIGINT handler for clean shutdown / quick restart
    signal(SIGINT, handle_sigint);

    // Start heartbeat thread to monitor storage server liveness
    pthread_t hb_thread; pthread_create(&hb_thread, NULL, storage_server_heartbeat_loop, NULL); pthread_detach(hb_thread);

    struct sockaddr_in server_addr;
    
    printf("=== LangOS Distributed File System - Name Server ===\n");
    log_message("NM", "INFO", "Starting Name Server on port %d", PORT_NM);
    
    // Initialize trie
    file_trie_root = create_trie_node();
    
    // Load persistent data
    load_persistent_data();
    
    // Create socket
    nm_socket = create_socket();
    if (nm_socket < 0) {
        log_message("NM", "ERROR", "Failed to create socket");
        exit(EXIT_FAILURE);
    }

    // Allow quick rebinding after Ctrl-C (TIME_WAIT)
    int opt = 1;
    setsockopt(nm_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind socket
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT_NM);
    
    if (bind(nm_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        log_message("NM", "ERROR", "Bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen
    if (listen(nm_socket, 50) < 0) {
        perror("Listen failed");
        log_message("NM", "ERROR", "Listen failed");
        exit(EXIT_FAILURE);
    }
    
    log_message("NM", "INFO", "Name Server listening for connections...");
    
    // NOTE: Keep persisted storage servers marked active so metadata ss_id mappings remain valid.
    // A server re-registering will refresh its entry; inactive detection can be added later.

    // Accept connections
    while (nm_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(nm_socket, (struct sockaddr*)&client_addr, &client_len);
        
        if (*client_sock < 0) {
            perror("Accept failed");
            free(client_sock);
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        log_message("NM", "INFO", "New connection from %s:%d", client_ip, ntohs(client_addr.sin_port));
        
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client_connection, client_sock) != 0) {
            perror("Thread creation failed");
            close(*client_sock);
            free(client_sock);
        } else {
            pthread_detach(thread);
        }
    }
    
    if (nm_socket > 0) close(nm_socket);
    trie_free(file_trie_root);
    return 0;
}

void* handle_client_connection(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);
    
    Message msg;
    
    while (1) {
        memset(&msg, 0, sizeof(Message));
        
        if (receive_message(client_sock, &msg) <= 0) {
            log_message("NM", "INFO", "Client disconnected");
            // Mark the client inactive based on socket_fd
            pthread_mutex_lock(&nm_lock);
            for (int i = 0; i < client_count; i++) {
                if (clients[i].socket_fd == client_sock) {
                    clients[i].active = 0;
                    break;
                }
            }
            int active_total = 0; for (int k=0;k<client_count;k++){ if (clients[k].active) active_total++; }
            log_message("NM", "INFO", "Active clients after disconnect: %d", active_total);
            pthread_mutex_unlock(&nm_lock);
            break;
        }
        
        log_request("NM", "client", client_sock, msg.username, "Operation");
        
        // Route based on operation
        switch (msg.op_code) {
            case OP_REGISTER_SS:
                register_storage_server(client_sock, &msg);
                break;
            case OP_REGISTER_CLIENT:
                register_client(client_sock, &msg);
                break;
            case OP_VIEW:
                handle_view_command(client_sock, &msg);
                break;
            case OP_VIEWFOLDER:
                handle_viewfolder_command(client_sock, &msg);
                break;
            case OP_CREATE:
                handle_create_command(client_sock, &msg);
                break;
            case OP_CREATEFOLDER:
                handle_createfolder_command(client_sock, &msg);
                break;
            case OP_DELETE:
                handle_delete_command(client_sock, &msg);
                break;
            case OP_MOVE:
                handle_move_command(client_sock, &msg);
                break;
            case OP_INFO:
                handle_info_command(client_sock, &msg);
                break;
            case OP_LIST:
                handle_list_command(client_sock, &msg);
                break;
            case OP_ADDACCESS:
                handle_addaccess_command(client_sock, &msg);
                break;
            case OP_REMACCESS:
                handle_remaccess_command(client_sock, &msg);
                break;
            case OP_REQACCESS:
                handle_reqaccess_command(client_sock, &msg);
                break;
            case OP_VIEWREQUESTS:
                handle_viewrequests_command(client_sock, &msg);
                break;
            case OP_APPROVE:
                handle_approve_command(client_sock, &msg);
                break;
            case OP_DENY:
                handle_deny_command(client_sock, &msg);
                break;
            case OP_EXEC:
                handle_exec_command(client_sock, &msg);
                break;
            case OP_READ:
            case OP_STREAM:
            case OP_UNDO:
            case OP_CHECKPOINT:
            case OP_VIEWCHECKPOINT:
            case OP_REVERT:
            case OP_LISTCHECKPOINTS:
                handle_read_stream_undo_command(client_sock, &msg);
                break;
            case OP_WRITE:
                handle_write_command(client_sock, &msg);
                break;
            case OP_RECENTS:
                handle_recents_command(client_sock, &msg);
                break;
            default:
                msg.error_code = ERR_INVALID_COMMAND;
                strcpy(msg.error_msg, "Invalid command");
                send_message(client_sock, &msg);
                break;
        }
    }
    
    close(client_sock);
    return NULL;
}

void register_storage_server(int socket_fd, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    // Parse registration data first
    char reg_ip[INET_ADDRSTRLEN]; int reg_nm_port = 0; int reg_client_port = 0;
    sscanf(msg->data, "%15s %d %d", reg_ip, &reg_nm_port, &reg_client_port);

    // Try to find existing inactive (or active) entry matching ip+ports to reuse
    StorageServerInfo* ss = NULL;
    for (int i = 0; i < ss_count; i++) {
        if (strcmp(storage_servers[i].ip, reg_ip) == 0 &&
            storage_servers[i].nm_port == reg_nm_port &&
            storage_servers[i].client_port == reg_client_port) {
            ss = &storage_servers[i];
            ss->active = 1; // Reactivate
            break;
        }
    }
    int was_inactive = 0;
    // If not found, append new entry
    if (ss == NULL) {
        if (ss_count >= MAX_SS) {
            msg->error_code = ERR_SERVER_ERROR;
            strcpy(msg->error_msg, "Maximum storage servers reached");
            pthread_mutex_unlock(&nm_lock);
            send_message(socket_fd, msg);
            return;
        }
        ss = &storage_servers[ss_count];
        ss->ss_id = ss_count;
        strncpy(ss->ip, reg_ip, sizeof(ss->ip)-1); ss->ip[sizeof(ss->ip)-1]='\0';
        ss->nm_port = reg_nm_port;
        ss->client_port = reg_client_port;
        ss->active = 1;
        // Keep existing file_count if this SS had files persisted; only zero if brand new
        if (ss->file_count < 0 || ss->file_count > MAX_FILES) ss->file_count = 0;
        ss_count++;
    }
    
    log_message("NM", "INFO", "Registered Storage Server %d: %s:%d (client_port: %d) active=%d", 
                ss->ss_id, ss->ip, ss->nm_port, ss->client_port, ss->active);
    
    msg->error_code = ERR_SUCCESS;
    sprintf(msg->data, "%d", ss->ss_id);
    
    pthread_mutex_unlock(&nm_lock);
    send_message(socket_fd, msg);
    save_persistent_data();

    // If this was a returning server (previously inactive), trigger resync
    if (was_inactive) {
        pthread_t sync_thread; int* sid = malloc(sizeof(int)); *sid = ss->ss_id; pthread_create(&sync_thread, NULL, sync_returned_primary, sid); pthread_detach(sync_thread);
    }

    // After registration, (re)announce replica partners to all active SS
    if (ss_count > 1) {
        for (int i = 0; i < ss_count; i++) {
            if (!storage_servers[i].active) continue;
            int partner = (i + 1) % ss_count;
            // find next active partner (simple linear search)
            int attempts = 0;
            while ((partner == i || !storage_servers[partner].active) && attempts < ss_count) {
                partner = (partner + 1) % ss_count;
                attempts++;
            }
            if (partner == i || !storage_servers[partner].active) continue; // no suitable partner
            Message ack; memset(&ack, 0, sizeof(ack));
            ack.op_code = OP_SS_ACK;
            // data: partner_ip partner_nm_port partner_client_port
            sprintf(ack.data, "%s %d %d", storage_servers[partner].ip, storage_servers[partner].nm_port, storage_servers[partner].client_port);
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons(storage_servers[i].nm_port);
                inet_pton(AF_INET, storage_servers[i].ip, &addr.sin_addr);
                if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    send_message(s, &ack);
                    // ignore response
                }
                close(s);
            }
        }
    }
}

void register_client(int socket_fd, Message* msg) {
    pthread_mutex_lock(&nm_lock);

    // If a client with the same username already exists, update it instead of adding duplicates
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].username, msg->username) == 0) {
            sscanf(msg->data, "%s %d %d", clients[i].ip, &clients[i].nm_port, &clients[i].ss_port);
            clients[i].socket_fd = socket_fd;
            clients[i].active = 1;
            int active_total = 0; for (int k=0;k<client_count;k++){ if (clients[k].active) active_total++; }
            log_message("NM", "INFO", "Re-registered Client: %s from %s:%d (active clients: %d)", clients[i].username, clients[i].ip, clients[i].nm_port, active_total);
            msg->error_code = ERR_SUCCESS;
            strcpy(msg->data, "Registration successful");
            pthread_mutex_unlock(&nm_lock);
            send_message(socket_fd, msg);
            return;
        }
    }

    if (client_count >= MAX_CLIENTS) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Maximum clients reached");
        pthread_mutex_unlock(&nm_lock);
        send_message(socket_fd, msg);
        return;
    }

    ClientInfo* client = &clients[client_count];
    strcpy(client->username, msg->username);
    sscanf(msg->data, "%s %d %d", client->ip, &client->nm_port, &client->ss_port);
    client->socket_fd = socket_fd;
    client->active = 1;

    client_count++;
    int active_total = 0; for (int k=0;k<client_count;k++){ if (clients[k].active) active_total++; }
    log_message("NM", "INFO", "Registered Client: %s from %s:%d (active clients: %d)", client->username, client->ip, client->nm_port, active_total);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Registration successful");

    pthread_mutex_unlock(&nm_lock);
    send_message(socket_fd, msg);
}

void handle_view_command(int client_sock, Message* msg) {
    int show_all = msg->flags & 1;  // -a flag
    int show_details = msg->flags & 2;  // -l flag
    
    pthread_mutex_lock(&nm_lock);
    
    char response[BUFFER_SIZE] = "";
    // To avoid showing duplicate filenames (from any prior inconsistent state),
    // keep track of what we've already emitted in this response.
    char seen[MAX_FILES][MAX_FILENAME];
    int seen_count = 0;
    
    if (show_details) {
        strcat(response, "---------------------------------------------------------\n");
        strcat(response, "|  Filename  | Words | Chars | Last Access Time | Owner |\n");
        strcat(response, "|------------|-------|-------|------------------|-------|\n");
    }
    
    // Iterate carefully since we may purge stale entries (modifies files[]/file_count)
    for (int i = 0; i < file_count; ) {
        FileMetadata* file = &files[i];
        
        // If file doesn't exist on SS anymore (stale), purge and continue without incrementing i
        int pre_count = file_count;
        int exists = ss_file_exists(file);
        if (exists == 0) {
            purge_file_metadata(file->filename);
            // If purge didn't change file_count (unexpected), advance to prevent infinite loop
            if (file_count == pre_count) {
                i++;
            }
            // After purge, files[i] now contains a new entry (swapped), so re-check same index if count changed
            continue;
        }
        // If SS unreachable, we don't purge metadata; just skip listing to avoid showing ghost files
        if (exists < 0) {
            i++;
            continue;
        }
        
        // Listing policy:
        // - If both primary and replica servers inactive -> hide file entirely (skip)
        int primary_active = (file->ss_id >=0 && file->ss_id < ss_count && storage_servers[file->ss_id].active);
        int replica_active = (file->replica_ss_id >=0 && file->replica_ss_id < ss_count && storage_servers[file->replica_ss_id].active);
        if (!primary_active && !replica_active) { i++; continue; }
        // VIEW / VIEW -l (without -a): show files where user is owner OR has been granted at least READ access.
        // Previously we only showed owner files; now we include any file for which access_list grants READ/WRITE.
        if (!show_all) {
            int is_owner = (strcmp(file->owner, msg->username) == 0);
            int has_access = is_owner || check_access(file, msg->username, ACCESS_READ);
            if (!has_access) { i++; continue; }
        }
        // Skip duplicates within this listing
        int dup = 0;
        for (int s = 0; s < seen_count; s++) {
            if (strcmp(seen[s], file->filename) == 0) { dup = 1; break; }
        }
        if (!dup) {
            strcpy(seen[seen_count++], file->filename);
            
            if (show_details) {
                char time_str[32];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", localtime(&file->accessed_time));
                char line[256];
                snprintf(line, sizeof(line), "| %-32s | %5d | %5d | %16s | %-12s |\n",
                        file->filename, file->word_count, file->char_count, time_str, file->owner);
                strcat(response, line);
            } else {
                strcat(response, "--> ");
                strcat(response, file->filename);
                strcat(response, "\n");
            }
        }
        i++;
    }
    
    if (show_details) {
        strcat(response, "-------------------------------------------------------------------------------------------------------------\n");
    }
    
    strcpy(msg->data, response);
    msg->error_code = ERR_SUCCESS;
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
    
    log_response("NM", "client", client_sock, ERR_SUCCESS, "VIEW command completed");
}

// Heartbeat thread: periodically probe SS nm_port to update active flag
void* storage_server_heartbeat_loop(void* arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&nm_lock);
        for (int i=0;i<ss_count;i++) {
            StorageServerInfo* ss = &storage_servers[i];
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { ss->active = 0; continue; }
            struct sockaddr_in addr; memset(&addr,0,sizeof(addr)); addr.sin_family=AF_INET; addr.sin_port=htons(ss->nm_port); inet_pton(AF_INET, ss->ip, &addr.sin_addr);
            int ok = (connect(s,(struct sockaddr*)&addr,sizeof(addr))==0);
            close(s);
            if (!ok && ss->active){
                ss->active = 0; log_message("NM","WARN","Heartbeat: SS %d marked inactive", ss->ss_id);
            } else if (ok && !ss->active){
                ss->active = 1; log_message("NM","INFO","Heartbeat: SS %d marked active", ss->ss_id);
                // Could trigger resync if becoming active spontaneously without registration path
            }
        }
        pthread_mutex_unlock(&nm_lock);
        sleep(5);
    }
    return NULL;
}

// Sync thread for a returning primary: copy replica contents back
void* sync_returned_primary(void* arg) {
    int ss_id = *(int*)arg; free(arg);
    pthread_mutex_lock(&nm_lock);
    // Collect files belonging to this primary
    FileMetadata local_files[MAX_FILES]; int count=0;
    for (int i=0;i<file_count;i++) {
        if (files[i].ss_id == ss_id && files[i].replica_ss_id >=0) {
            local_files[count++] = files[i];
        }
    }
    pthread_mutex_unlock(&nm_lock);
    if (count==0) return NULL;
    log_message("NM","INFO","Sync: primary SS %d returning, syncing %d files from replicas", ss_id, count);
    for (int i=0;i<count;i++) {
        FileMetadata* f = &local_files[i];
        // Read content from replica
        if (f->replica_ss_id <0 || f->replica_ss_id >= ss_count) continue;
        StorageServerInfo* rss = &storage_servers[f->replica_ss_id];
        int rs = socket(AF_INET, SOCK_STREAM, 0);
        if (rs < 0) continue;
        struct sockaddr_in raddr; memset(&raddr,0,sizeof(raddr)); raddr.sin_family=AF_INET; raddr.sin_port=htons(rss->nm_port); inet_pton(AF_INET, rss->ip, &raddr.sin_addr);
        if (connect(rs,(struct sockaddr*)&raddr,sizeof(raddr))!=0){ close(rs); continue; }
        Message req; memset(&req,0,sizeof(req)); req.op_code=OP_READ; strncpy(req.filename,f->filename,sizeof(req.filename)-1); send_message(rs,&req); receive_message(rs,&req); close(rs);
        if (req.error_code!=ERR_SUCCESS) continue;
        // Write content to primary (replication write)
        StorageServerInfo* pss = &storage_servers[f->ss_id];
        int ps = socket(AF_INET, SOCK_STREAM, 0); if (ps<0) continue;
        struct sockaddr_in paddr; memset(&paddr,0,sizeof(paddr)); paddr.sin_family=AF_INET; paddr.sin_port=htons(pss->nm_port); inet_pton(AF_INET, pss->ip, &paddr.sin_addr);
        if (connect(ps,(struct sockaddr*)&paddr,sizeof(paddr))!=0){ close(ps); continue; }
        Message w; memset(&w,0,sizeof(w)); w.op_code=OP_REPL_WRITE; strncpy(w.filename,f->filename,sizeof(w.filename)-1); strncpy(w.data,req.data,sizeof(w.data)-1); send_message(ps,&w); receive_message(ps,&w); close(ps);
    }
    log_message("NM","INFO","Sync: completed for primary SS %d", ss_id);
    return NULL;
}

void handle_create_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    // Check if file exists
    if (trie_search(file_trie_root, msg->filename) != NULL) {
        msg->error_code = ERR_FILE_EXISTS;
        strcpy(msg->error_msg, "File already exists");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Find an available storage server
    if (ss_count == 0) {
        msg->error_code = ERR_SS_NOT_FOUND;
        strcpy(msg->error_msg, "No storage servers available");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Try to create on an active storage server. Start from round-robin index but probe others if needed.
    int start = (file_count < ss_count) ? (file_count % ss_count) : (file_count % ss_count);
    int chosen = -1;
    Message ss_reply;
    for (int attempt = 0; attempt < ss_count; attempt++) {
        int idx = (start + attempt) % ss_count;
        StorageServerInfo* cand = &storage_servers[idx];
        if (!cand->active) continue; // skip inactive

        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ss_sock < 0) continue;
        struct sockaddr_in ss_addr; memset(&ss_addr, 0, sizeof(ss_addr));
        ss_addr.sin_family = AF_INET; ss_addr.sin_port = htons(cand->nm_port);
        inet_pton(AF_INET, cand->ip, &ss_addr.sin_addr);
        if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
            // Send create to this SS
            Message req = *msg; // includes filename/username/op already
            send_message(ss_sock, &req);
            memset(&ss_reply, 0, sizeof(ss_reply));
            if (receive_message(ss_sock, &ss_reply) > 0 && ss_reply.error_code == ERR_SUCCESS) {
                chosen = idx;
                close(ss_sock);
                break;
            }
        }
        close(ss_sock);
    }

    if (chosen < 0) {
        msg->error_code = ERR_CONNECTION_FAILED;
        strcpy(msg->error_msg, "Failed to connect to storage server");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }

    // Persist metadata only after SS confirmed creation
    StorageServerInfo* ss = &storage_servers[chosen];
    FileMetadata* file = &files[file_count];
    strcpy(file->filename, msg->filename);
    strcpy(file->owner, msg->username);
    file->ss_id = ss->ss_id;
    strcpy(file->ss_ip, ss->ip);
    file->ss_port = ss->client_port;
    // Set replica to next ACTIVE SS if exists
    int replica_id = -1; int replica_idx = -1;
    if (ss_count > 1) {
        for (int off = 1; off <= ss_count; off++) {
            int j = (chosen + off) % ss_count;
            if (storage_servers[j].active && j != chosen) { replica_idx = j; break; }
        }
    }
    if (replica_idx >= 0) {
        StorageServerInfo* rss = &storage_servers[replica_idx];
        replica_id = rss->ss_id;
        file->replica_ss_id = replica_id;
        strcpy(file->replica_ss_ip, rss->ip);
        file->replica_ss_port = rss->client_port;
    } else {
        file->replica_ss_id = -1; file->replica_ss_ip[0] = '\0'; file->replica_ss_port = 0;
    }
    file->access_count = 0;
    file->created_time = time(NULL);
    file->modified_time = time(NULL);
    file->accessed_time = time(NULL);
    file->size = 0;
    file->word_count = 0;
    file->char_count = 0;
    strcpy(file->last_accessed_by, msg->username);

    trie_insert(file_trie_root, msg->filename, file);
    // Add to SS file list for chosen
    strcpy(ss->files[ss->file_count], msg->filename);
    ss->file_count++;
    file_count++;

    // Return success to client
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File created successfully");
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);

    save_persistent_data();
    log_message("NM", "INFO", "File created: %s by %s on SS %d", msg->filename, msg->username, ss->ss_id);
}

void handle_delete_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Check if user is owner
    if (strcmp(file->owner, msg->username) != 0) {
        msg->error_code = ERR_NOT_OWNER;
        strcpy(msg->error_msg, "Only the owner can delete the file");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Forward to storage server
    StorageServerInfo* ss = &storage_servers[file->ss_id];
    
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock >= 0) {
        struct sockaddr_in ss_addr;
        ss_addr.sin_family = AF_INET;
        ss_addr.sin_port = htons(ss->nm_port);
        inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr);
        
        if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) >= 0) {
            send_message(ss_sock, msg);
            receive_message(ss_sock, msg);
        } else {
            // If we couldn't reach SS, return connection failed and do not mutate NM state
            msg->error_code = ERR_CONNECTION_FAILED;
            strcpy(msg->error_msg, "Failed to connect to storage server");
        }
        close(ss_sock);
    } else {
        msg->error_code = ERR_CONNECTION_FAILED;
        strcpy(msg->error_msg, "Failed to create socket to storage server");
    }

    // If SS deletion failed, abort here without touching NM metadata
    if (msg->error_code != ERR_SUCCESS) {
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }

    // Purge metadata now that SS deletion succeeded
    purge_file_metadata(msg->filename);

    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "File deleted successfully");
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
    
    save_persistent_data();
    log_message("NM", "INFO", "File deleted: %s by %s", msg->filename, msg->username);
}

void handle_info_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = search_file_cached(msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Validate existence on SS; if missing, purge metadata and report not found
    int exists = ss_file_exists(file);
    if (exists == 0) {
        purge_file_metadata(file->filename);
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    // If SS unreachable, fall through to return metadata we have (avoid blocking all INFO)
    
    char response[BUFFER_SIZE];
    char created_time[64], modified_time[64], accessed_time[64];
    
    strftime(created_time, sizeof(created_time), "%Y-%m-%d %H:%M", localtime(&file->created_time));
    strftime(modified_time, sizeof(modified_time), "%Y-%m-%d %H:%M", localtime(&file->modified_time));
    strftime(accessed_time, sizeof(accessed_time), "%Y-%m-%d %H:%M", localtime(&file->accessed_time));
    
    snprintf(response, sizeof(response),
            "--> File: %s\n"
            "--> Owner: %s\n"
            "--> Created: %s\n"
            "--> Last Modified: %s\n"
            "--> Size: %ld bytes\n"
            "--> Access: %s (RW)",
            file->filename, file->owner, created_time, modified_time, file->size, file->owner);
    
    for (int i = 0; i < file->access_count; i++) {
        char access_str[128];
        snprintf(access_str, sizeof(access_str), ", %s (%s)",
                file->access_list[i].username,
                file->access_list[i].access_type == ACCESS_WRITE ? "RW" : "R");
        strcat(response, access_str);
    }
    
    char last_access[128];
    snprintf(last_access, sizeof(last_access), "\n--> Last Accessed: %s by %s",
            accessed_time, file->last_accessed_by);
    strcat(response, last_access);
    
    strcpy(msg->data, response);
    msg->error_code = ERR_SUCCESS;
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

void handle_list_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    char response[BUFFER_SIZE] = "";
    
    for (int i = 0; i < client_count; i++) {
        if (clients[i].active) {
            strcat(response, "--> ");
            strcat(response, clients[i].username);
            strcat(response, "\n");
        }
    }
    
    strcpy(msg->data, response);
    msg->error_code = ERR_SUCCESS;
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

// BONUS: CREATEFOLDER - instruct all SS to mkdir the folder path under their storage_dir
void handle_createfolder_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    char folder[MAX_FILENAME];
    strncpy(folder, msg->filename, sizeof(folder)-1);
    folder[sizeof(folder)-1] = '\0';
    int successes = 0;
    for (int i = 0; i < ss_count; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET; addr.sin_port = htons(storage_servers[i].nm_port);
        inet_pton(AF_INET, storage_servers[i].ip, &addr.sin_addr);
        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            Message m; memset(&m, 0, sizeof(m));
            m.op_code = OP_CREATEFOLDER;
            strncpy(m.filename, folder, sizeof(m.filename)-1);
            send_message(s, &m);
            receive_message(s, &m);
            if (m.error_code == ERR_SUCCESS) successes++;
        }
        close(s);
    }
    pthread_mutex_unlock(&nm_lock);
    if (successes > 0) {
        msg->error_code = ERR_SUCCESS;
        strcpy(msg->data, "Folder created");
    } else {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Failed to create folder");
    }
    send_message(client_sock, msg);
}

// BONUS: VIEWFOLDER - list files under a given folder prefix
void handle_viewfolder_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    char folder[MAX_FILENAME];
    strncpy(folder, msg->filename, sizeof(folder)-1);
    folder[sizeof(folder)-1] = '\0';
    char prefix[MAX_FILENAME+2];
    snprintf(prefix, sizeof(prefix), "%s/", folder);
    size_t plen = strlen(prefix);
    char response[BUFFER_SIZE] = "";
    for (int i = 0; i < file_count; i++) {
        FileMetadata* f = &files[i];
        if (strncmp(f->filename, prefix, plen) == 0) {
            if (!check_access(f, msg->username, ACCESS_READ)) continue;
            strcat(response, "--> ");
            // Show leaf name after folder/
            const char* leaf = f->filename + plen;
            strcat(response, leaf);
            strcat(response, "\n");
        }
    }
    pthread_mutex_unlock(&nm_lock);
    msg->error_code = ERR_SUCCESS;
    strncpy(msg->data, response, sizeof(msg->data)-1);
    send_message(client_sock, msg);
}

// BONUS: MOVE - move a file into a folder (updates metadata and ask SS to rename)
static const char* basename_const(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void handle_move_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    if (!file) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    if (strcmp(file->owner, msg->username) != 0) {
        msg->error_code = ERR_NOT_OWNER;
        strcpy(msg->error_msg, "Only owner can move file");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    char folder[MAX_FILENAME]; strncpy(folder, msg->data, sizeof(folder)-1); folder[sizeof(folder)-1] = '\0';
    char newname[MAX_FILENAME];
    snprintf(newname, sizeof(newname), "%s/%s", folder, basename_const(file->filename));
    // Ask SS to move first
    StorageServerInfo* ss = &storage_servers[file->ss_id];
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET; addr.sin_port = htons(ss->nm_port);
        inet_pton(AF_INET, ss->ip, &addr.sin_addr);
        if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            Message m; memset(&m, 0, sizeof(m));
            m.op_code = OP_MOVE; strncpy(m.filename, file->filename, sizeof(m.filename)-1);
            strncpy(m.data, newname, sizeof(m.data)-1);
            send_message(s, &m);
            receive_message(s, &m);
            close(s);
            if (m.error_code != ERR_SUCCESS) {
                msg->error_code = m.error_code;
                strncpy(msg->error_msg, m.error_msg, sizeof(msg->error_msg)-1);
                pthread_mutex_unlock(&nm_lock);
                send_message(client_sock, msg);
                return;
            }
            // Best-effort replicate MOVE to replica SS (synchronous to ensure path consistency if partner exists)
            if (file->replica_ss_port > 0 && strlen(file->replica_ss_ip) > 0) {
                int rs = socket(AF_INET, SOCK_STREAM, 0);
                if (rs >= 0) {
                    struct sockaddr_in raddr; memset(&raddr, 0, sizeof(raddr));
                    raddr.sin_family = AF_INET; raddr.sin_port = htons(storage_servers[file->replica_ss_id].nm_port);
                    inet_pton(AF_INET, storage_servers[file->replica_ss_id].ip, &raddr.sin_addr);
                    if (connect(rs, (struct sockaddr*)&raddr, sizeof(raddr)) == 0) {
                        Message rm; memset(&rm, 0, sizeof(rm));
                        rm.op_code = OP_MOVE; // use normal MOVE so replica renames and its own code can replicate if chain exists
                        strncpy(rm.filename, file->filename, sizeof(rm.filename)-1);
                        strncpy(rm.data, newname, sizeof(rm.data)-1);
                        send_message(rs, &rm);
                        // ignore response errors best-effort
                        receive_message(rs, &rm);
                    }
                    close(rs);
                }
            }
        } else {
            close(s);
            msg->error_code = ERR_CONNECTION_FAILED;
            strcpy(msg->error_msg, "Failed to connect to storage server");
            pthread_mutex_unlock(&nm_lock);
            send_message(client_sock, msg);
            return;
        }
    }
    // Update NM metadata and trie
    char oldname[MAX_FILENAME]; strncpy(oldname, file->filename, sizeof(oldname)-1); oldname[sizeof(oldname)-1] = '\0';
    trie_delete(file_trie_root, oldname);
    strncpy(file->filename, newname, sizeof(file->filename)-1);
    trie_insert(file_trie_root, file->filename, file);
    // Also update SS file list entry
    StorageServerInfo* ssp = &storage_servers[file->ss_id];
    for (int i = 0; i < ssp->file_count; i++) {
        if (strcmp(ssp->files[i], oldname) == 0) { strncpy(ssp->files[i], newname, MAX_FILENAME-1); ssp->files[i][MAX_FILENAME-1]='\0'; break; }
    }
    save_persistent_data();
    pthread_mutex_unlock(&nm_lock);
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Move successful");
    send_message(client_sock, msg);
}

// BONUS: Access requests
void handle_reqaccess_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    if (!file) { msg->error_code = ERR_FILE_NOT_FOUND; strcpy(msg->error_msg, "File not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    if (strcmp(file->owner, msg->username) == 0) { msg->error_code = ERR_INVALID_COMMAND; strcpy(msg->error_msg, "Owner already has full access"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    // If already has access, ignore
    if (check_access(file, msg->username, (msg->flags & 1) ? ACCESS_WRITE : ACCESS_READ)) {
        msg->error_code = ERR_SUCCESS; strcpy(msg->data, "Already has access"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return;
    }
    // Add pending if not present
    for (int i=0;i<file->pending_count;i++){ if (strcmp(file->pending_requests[i].username, msg->username)==0){ msg->error_code=ERR_SUCCESS; strcpy(msg->data, "Request already pending"); pthread_mutex_unlock(&nm_lock); send_message(client_sock,msg); return; }}
    if (file->pending_count < MAX_ACCESS_LIST) {
        strcpy(file->pending_requests[file->pending_count].username, msg->username);
        file->pending_requests[file->pending_count].access_type = (msg->flags & 1) ? ACCESS_WRITE : ACCESS_READ;
        file->pending_count++;
        save_persistent_data();
        msg->error_code = ERR_SUCCESS; strcpy(msg->data, "Access request submitted");
    } else { msg->error_code = ERR_SERVER_ERROR; strcpy(msg->error_msg, "Too many pending requests"); }
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

void handle_viewrequests_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    if (!file) { msg->error_code = ERR_FILE_NOT_FOUND; strcpy(msg->error_msg, "File not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    if (strcmp(file->owner, msg->username) != 0) { msg->error_code = ERR_NOT_OWNER; strcpy(msg->error_msg, "Only owner can view requests"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    char resp[BUFFER_SIZE] = "";
    for (int i=0;i<file->pending_count;i++) {
        char line[128]; snprintf(line, sizeof(line), "--> %s (%s)\n", file->pending_requests[i].username, file->pending_requests[i].access_type==ACCESS_WRITE?"W":"R");
        strcat(resp, line);
    }
    msg->error_code = ERR_SUCCESS; strncpy(msg->data, resp, sizeof(msg->data)-1);
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

void handle_approve_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    if (!file) { msg->error_code = ERR_FILE_NOT_FOUND; strcpy(msg->error_msg, "File not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    if (strcmp(file->owner, msg->username) != 0) { msg->error_code = ERR_NOT_OWNER; strcpy(msg->error_msg, "Only owner can approve"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    char target[MAX_USERNAME]; int want_write = (msg->flags & 1);
    sscanf(msg->data, "%63s", target);
    int idx=-1; for (int i=0;i<file->pending_count;i++){ if(strcmp(file->pending_requests[i].username,target)==0){ idx=i; break; }}
    if (idx<0){ msg->error_code=ERR_USER_NOT_FOUND; strcpy(msg->error_msg, "Request not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock,msg); return; }
    int grant = want_write ? ACCESS_WRITE : file->pending_requests[idx].access_type;
    // Update or add access entry
    int aidx=-1; for(int i=0;i<file->access_count;i++){ if(strcmp(file->access_list[i].username,target)==0){ aidx=i; break; }}
    if (aidx>=0){ file->access_list[aidx].access_type = grant; } else if (file->access_count<MAX_ACCESS_LIST){ strcpy(file->access_list[file->access_count].username,target); file->access_list[file->access_count].access_type=grant; file->access_count++; }
    // Remove pending
    for (int j=idx;j<file->pending_count-1;j++){ file->pending_requests[j]=file->pending_requests[j+1]; }
    file->pending_count--;
    save_persistent_data();
    msg->error_code=ERR_SUCCESS; strcpy(msg->data, "Approved");
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

void handle_deny_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    if (!file) { msg->error_code = ERR_FILE_NOT_FOUND; strcpy(msg->error_msg, "File not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    if (strcmp(file->owner, msg->username) != 0) { msg->error_code = ERR_NOT_OWNER; strcpy(msg->error_msg, "Only owner can deny"); pthread_mutex_unlock(&nm_lock); send_message(client_sock, msg); return; }
    char target[MAX_USERNAME]; sscanf(msg->data, "%63s", target);
    int idx=-1; for (int i=0;i<file->pending_count;i++){ if(strcmp(file->pending_requests[i].username,target)==0){ idx=i; break; }}
    if (idx<0){ msg->error_code=ERR_USER_NOT_FOUND; strcpy(msg->error_msg, "Request not found"); pthread_mutex_unlock(&nm_lock); send_message(client_sock,msg); return; }
    for (int j=idx;j<file->pending_count-1;j++){ file->pending_requests[j]=file->pending_requests[j+1]; }
    file->pending_count--;
    save_persistent_data();
    msg->error_code=ERR_SUCCESS; strcpy(msg->data, "Denied");
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

// BONUS: RECENTS - list last 5 files accessed by user
void handle_recents_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    // Collect candidates
    int idxs[100]; int cnt=0;
    for (int i=0;i<file_count;i++) {
        if (check_access(&files[i], msg->username, ACCESS_READ)) {
            idxs[cnt++] = i; if (cnt>=100) break;
        }
    }
    // Partial sort by accessed_time desc (simple selection of top 5)
    int top = cnt < 5 ? cnt : 5;
    for (int i=0;i<top;i++){
        int best=i; for(int j=i+1;j<cnt;j++){ if(files[idxs[j]].accessed_time > files[idxs[best]].accessed_time) best=j; }
        int tmp=idxs[i]; idxs[i]=idxs[best]; idxs[best]=tmp;
    }
    char resp[BUFFER_SIZE] = "";
    for (int i=0;i<top;i++){
        char line[256];
        char time_str[32]; strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", localtime(&files[idxs[i]].accessed_time));
        snprintf(line, sizeof(line), "--> %s (last: %s)\n", files[idxs[i]].filename, time_str);
        strcat(resp, line);
    }
    pthread_mutex_unlock(&nm_lock);
    msg->error_code = ERR_SUCCESS; strncpy(msg->data, resp, sizeof(msg->data)-1);
    send_message(client_sock, msg);
}

void handle_addaccess_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    if (strcmp(file->owner, msg->username) != 0) {
        msg->error_code = ERR_NOT_OWNER;
        strcpy(msg->error_msg, "Only the owner can grant access");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    char target_user[MAX_USERNAME];
    sscanf(msg->data, "%s", target_user);
    
    // Check if user exists
    int user_exists = 0;
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].username, target_user) == 0) {
            user_exists = 1;
            break;
        }
    }
    
    if (!user_exists) {
        msg->error_code = ERR_USER_NOT_FOUND;
        strcpy(msg->error_msg, "User not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Check if already has access
    int access_index = -1;
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, target_user) == 0) {
            access_index = i;
            break;
        }
    }
    
    int access_type = (msg->flags & 1) ? ACCESS_WRITE : ACCESS_READ;  // -W or -R
    
    if (access_index >= 0) {
        file->access_list[access_index].access_type = access_type;
    } else {
        if (file->access_count < MAX_ACCESS_LIST) {
            strcpy(file->access_list[file->access_count].username, target_user);
            file->access_list[file->access_count].access_type = access_type;
            file->access_count++;
        }
    }
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Access granted successfully");
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
    
    save_persistent_data();
    log_message("NM", "INFO", "Access granted to %s for file %s", target_user, msg->filename);
}

void handle_remaccess_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    if (strcmp(file->owner, msg->username) != 0) {
        msg->error_code = ERR_NOT_OWNER;
        strcpy(msg->error_msg, "Only the owner can remove access");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    char target_user[MAX_USERNAME];
    sscanf(msg->data, "%s", target_user);
    
    // Remove access
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, target_user) == 0) {
            for (int j = i; j < file->access_count - 1; j++) {
                file->access_list[j] = file->access_list[j + 1];
            }
            file->access_count--;
            break;
        }
    }
    
    msg->error_code = ERR_SUCCESS;
    strcpy(msg->data, "Access removed successfully");
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
    
    save_persistent_data();
    log_message("NM", "INFO", "Access removed from %s for file %s", target_user, msg->filename);
}

void handle_exec_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    if (!check_access(file, msg->username, ACCESS_READ)) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->error_msg, "Access denied");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Get file content from SS
    StorageServerInfo* ss = &storage_servers[file->ss_id];
    
    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) {
        msg->error_code = ERR_CONNECTION_FAILED;
        strcpy(msg->error_msg, "Failed to connect to storage server");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss->nm_port);
    inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr);
    
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        msg->error_code = ERR_CONNECTION_FAILED;
        strcpy(msg->error_msg, "Failed to connect to storage server");
        close(ss_sock);
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    Message ss_msg = *msg;
    ss_msg.op_code = OP_READ;
    send_message(ss_sock, &ss_msg);
    receive_message(ss_sock, &ss_msg);
    close(ss_sock);
    
    pthread_mutex_unlock(&nm_lock);
    
    if (ss_msg.error_code != ERR_SUCCESS) {
        send_message(client_sock, &ss_msg);
        return;
    }
    
    // Execute commands
    FILE* temp_file = tmpfile();
    if (temp_file == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Failed to create temporary file");
        send_message(client_sock, msg);
        return;
    }
    
    fprintf(temp_file, "%s", ss_msg.data);
    fflush(temp_file);
    rewind(temp_file);
    
    char command[MAX_COMMAND];
    char output[BUFFER_SIZE] = "";
    
    while (fgets(command, sizeof(command), temp_file)) {
        command[strcspn(command, "\n")] = 0;
        
        FILE* pipe = popen(command, "r");
        if (pipe) {
            char line[1024];
            while (fgets(line, sizeof(line), pipe)) {
                strcat(output, line);
            }
            pclose(pipe);
        }
    }
    
    fclose(temp_file);
    
    strcpy(msg->data, output);
    msg->error_code = ERR_SUCCESS;
    send_message(client_sock, msg);
    
    log_message("NM", "INFO", "Executed file: %s by %s", msg->filename, msg->username);
}

void handle_read_stream_undo_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = search_file_cached(msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    if (msg->op_code == OP_READ || msg->op_code == OP_STREAM || msg->op_code == OP_VIEWCHECKPOINT || msg->op_code == OP_LISTCHECKPOINTS) {
        if (!check_access(file, msg->username, ACCESS_READ)) {
            msg->error_code = ERR_ACCESS_DENIED;
            strcpy(msg->error_msg, "Access denied");
            pthread_mutex_unlock(&nm_lock);
            send_message(client_sock, msg);
            return;
        }
    }
    if (msg->op_code == OP_UNDO || msg->op_code == OP_REVERT || msg->op_code == OP_CHECKPOINT) {
        if (!check_access(file, msg->username, ACCESS_WRITE)) {
            msg->error_code = ERR_ACCESS_DENIED;
            strcpy(msg->error_msg, "Access denied");
            pthread_mutex_unlock(&nm_lock);
            send_message(client_sock, msg);
            return;
        }
    }
    
    // Return SS connection info with fallback to replica if primary unreachable
    SSConnection ss_conn;
    strcpy(ss_conn.ss_ip, file->ss_ip);
    ss_conn.ss_port = file->ss_port;
    
    // Probe primary client port; if unreachable and replica exists, use replica
    int probe_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (probe_sock >= 0) {
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ss_conn.ss_port);
        inet_pton(AF_INET, ss_conn.ss_ip, &addr.sin_addr);
        if (connect(probe_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            // try replica
            if (file->replica_ss_port > 0 && strlen(file->replica_ss_ip) > 0) {
                strcpy(ss_conn.ss_ip, file->replica_ss_ip);
                ss_conn.ss_port = file->replica_ss_port;
            }
        }
        close(probe_sock);
    }
    
    strcpy(msg->data, "");
    sprintf(msg->data, "%s %d", ss_conn.ss_ip, ss_conn.ss_port);
    msg->error_code = ERR_SUCCESS;
    
    file->accessed_time = time(NULL);
    strcpy(file->last_accessed_by, msg->username);
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

void handle_write_command(int client_sock, Message* msg) {
    pthread_mutex_lock(&nm_lock);
    
    FileMetadata* file = trie_search(file_trie_root, msg->filename);
    
    if (file == NULL) {
        msg->error_code = ERR_FILE_NOT_FOUND;
        strcpy(msg->error_msg, "File not found");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    if (!check_access(file, msg->username, ACCESS_WRITE)) {
        msg->error_code = ERR_ACCESS_DENIED;
        strcpy(msg->error_msg, "Access denied");
        pthread_mutex_unlock(&nm_lock);
        send_message(client_sock, msg);
        return;
    }
    
    // Return SS connection info with fallback to replica if primary client port unreachable
    SSConnection ss_conn;
    strcpy(ss_conn.ss_ip, file->ss_ip);
    ss_conn.ss_port = file->ss_port;

    int probe_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (probe_sock >= 0) {
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET; addr.sin_port = htons(ss_conn.ss_port);
        inet_pton(AF_INET, ss_conn.ss_ip, &addr.sin_addr);
        if (connect(probe_sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            // try replica
            if (file->replica_ss_port > 0 && strlen(file->replica_ss_ip) > 0) {
                strcpy(ss_conn.ss_ip, file->replica_ss_ip);
                ss_conn.ss_port = file->replica_ss_port;
            }
        }
        close(probe_sock);
    }

    sprintf(msg->data, "%s %d", ss_conn.ss_ip, ss_conn.ss_port);
    msg->error_code = ERR_SUCCESS;
    
    file->modified_time = time(NULL);
    file->accessed_time = time(NULL);
    strcpy(file->last_accessed_by, msg->username);
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
}

FileMetadata* search_file_cached(const char* filename) {
    // Check cache first
    time_t now = time(NULL);
    for (int i = 0; i < cache_size; i++) {
        if (strcmp(search_cache[i].filename, filename) == 0) {
            if (difftime(now, search_cache[i].timestamp) < 60) {  // Cache valid for 60 seconds
                return search_cache[i].file_info;
            }
        }
    }
    
    // Search in trie
    FileMetadata* file = trie_search(file_trie_root, filename);
    if (file != NULL) {
        update_cache(filename, file);
    }
    
    return file;
}

void update_cache(const char* filename, FileMetadata* file_info) {
    if (cache_size < 100) {
        strcpy(search_cache[cache_size].filename, filename);
        search_cache[cache_size].file_info = file_info;
        search_cache[cache_size].timestamp = time(NULL);
        cache_size++;
    } else {
        // Replace oldest entry
        int oldest = 0;
        for (int i = 1; i < 100; i++) {
            if (search_cache[i].timestamp < search_cache[oldest].timestamp) {
                oldest = i;
            }
        }
        strcpy(search_cache[oldest].filename, filename);
        search_cache[oldest].file_info = file_info;
        search_cache[oldest].timestamp = time(NULL);
    }
}

void load_persistent_data() {
    FILE* fp = fopen("nm_data.dat", "rb");
    if (fp == NULL) {
        log_message("NM", "INFO", "No persistent data found, starting fresh");
        return;
    }
    
    // Validate file size and read counts defensively
    int rc;
    rc = fread(&file_count, sizeof(int), 1, fp);
    if (rc != 1 || file_count < 0 || file_count > MAX_FILES) {
        fclose(fp);
        file_count = 0; ss_count = 0;
        log_message("NM", "ERROR", "Corrupt nm_data.dat (file_count). Starting fresh");
        save_persistent_data();
        return;
    }
    rc = fread(files, sizeof(FileMetadata), file_count, fp);
    if (rc != file_count) {
        fclose(fp);
        file_count = 0; ss_count = 0;
        log_message("NM", "ERROR", "Corrupt nm_data.dat (files array). Starting fresh");
        save_persistent_data();
        return;
    }
    rc = fread(&ss_count, sizeof(int), 1, fp);
    if (rc != 1 || ss_count < 0 || ss_count > MAX_SS) {
        fclose(fp);
        ss_count = 0;
        log_message("NM", "ERROR", "Corrupt nm_data.dat (ss_count); continuing with 0 storage servers");
        save_persistent_data();
        // continue with files only
        fp = NULL;
    }
    if (fp) {
        rc = fread(storage_servers, sizeof(StorageServerInfo), ss_count, fp);
        if (rc != ss_count) {
            log_message("NM", "ERROR", "Corrupt nm_data.dat (storage_servers array); zeroing SS list");
            ss_count = 0;
        }
    }
    
    // Rebuild trie with de-duplication to recover from any stale entries
    int new_count = 0;
    time_t now = time(NULL);
    for (int i = 0; i < file_count; i++) {
        // Basic validation of each record; skip invalid ones
        files[i].filename[MAX_FILENAME-1] = '\0';
        files[i].owner[MAX_USERNAME-1] = '\0';
        if (files[i].filename[0] == '\0' || files[i].owner[0] == '\0') continue;
        if (files[i].access_count < 0 || files[i].access_count > MAX_ACCESS_LIST) files[i].access_count = 0;
        if (files[i].char_count < 0) files[i].char_count = 0;
        if (files[i].word_count < 0) files[i].word_count = 0;
        if (files[i].ss_id < 0 || files[i].ss_id >= MAX_SS) files[i].ss_id = 0;

        int exists = 0;
        for (int j = 0; j < new_count; j++) {
            if (strcmp(files[j].filename, files[i].filename) == 0) { exists = 1; break; }
        }
        if (!exists) {
            if (i != new_count) {
                files[new_count] = files[i];
            }
            // Initialize any zero timestamps (legacy persisted entries) to current time
            if (files[new_count].created_time == 0) files[new_count].created_time = now;
            if (files[new_count].modified_time == 0) files[new_count].modified_time = files[new_count].created_time;
            if (files[new_count].accessed_time == 0) files[new_count].accessed_time = files[new_count].modified_time;
            trie_insert(file_trie_root, files[new_count].filename, &files[new_count]);
            new_count++;
        }
    }
    file_count = new_count;
    
    fclose(fp);
    log_message("NM", "INFO", "Loaded %d files and %d storage servers from persistent storage", file_count, ss_count);
}

void save_persistent_data() {
    FILE* fp = fopen("nm_data.dat", "wb");
    if (fp == NULL) {
        log_message("NM", "ERROR", "Failed to save persistent data");
        return;
    }
    
    fwrite(&file_count, sizeof(int), 1, fp);
    fwrite(files, sizeof(FileMetadata), file_count, fp);
    fwrite(&ss_count, sizeof(int), 1, fp);
    fwrite(storage_servers, sizeof(StorageServerInfo), ss_count, fp);
    
    fclose(fp);
}

// Connect to SS (NM port) and check if file exists by attempting a lightweight READ
// Returns: 1 = exists, 0 = not found (purge candidate), -1 = SS unreachable/error
static int ss_file_exists(FileMetadata* file) {
    if (file == NULL) return 0;
    int tried_replica = 0;
    int which_ss = file->ss_id;
    if (which_ss < 0 || which_ss >= ss_count) return -1;
retry_probe:
    StorageServerInfo* ss = &storage_servers[which_ss];

    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) return -1;

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss->nm_port);
    if (inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr) != 1) {
        close(ss_sock);
        return -1;
    }
    // Set a short timeout to avoid blocking VIEW if SS is down
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 300000; // 300ms
    setsockopt(ss_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ss_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_sock);
        // if primary failed and replica exists, probe replica
        if (!tried_replica && file->replica_ss_id >= 0 && file->replica_ss_id < ss_count) {
            tried_replica = 1; which_ss = file->replica_ss_id; goto retry_probe;
        }
        return -1;
    }

    Message m; memset(&m, 0, sizeof(m));
    m.op_code = OP_READ; // SS NM listener supports OP_READ
    strncpy(m.filename, file->filename, sizeof(m.filename)-1);
    strncpy(m.username, "NM", sizeof(m.username)-1);
    send_message(ss_sock, &m);
    if (receive_message(ss_sock, &m) <= 0) { close(ss_sock); return -1; }
    close(ss_sock);

    if (m.error_code == ERR_SUCCESS) {
        // Update metadata counts opportunistically using returned content
        // Count chars and words in m.data
        int chars = (int)strlen(m.data);
        int words = 0; int inw = 0;
        for (int i=0; i<chars; i++) {
            if (!isspace((unsigned char)m.data[i])) { if (!inw) { words++; inw=1; } }
            else { inw=0; }
        }
        file->char_count = chars;
        file->word_count = words;
        return 1;
    }
    if (m.error_code == ERR_FILE_NOT_FOUND) return 0;
    return -1;
}

// Remove all traces of a filename from NM memory (trie, arrays, SS lists) and reset cache
static void purge_file_metadata(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return;

    // Remove from trie (primary index) — safe even if not present
    trie_delete(file_trie_root, filename);

    // Remove from ALL SS file lists (clean up any stale entries)
    for (int s = 0; s < ss_count; s++) {
        StorageServerInfo* ssp = &storage_servers[s];
        for (int i = 0; i < ssp->file_count; i++) {
            if (strcmp(ssp->files[i], filename) == 0) {
                for (int j = i; j < ssp->file_count - 1; j++) {
                    strcpy(ssp->files[j], ssp->files[j + 1]);
                }
                ssp->file_count--;
                i--; // stay at same index after shift
            }
        }
    }

    // Remove ALL occurrences from files[] and keep array compact
    int i = 0;
    while (i < file_count) {
        if (strcmp(files[i].filename, filename) == 0) {
            int last = file_count - 1;
            if (i != last) {
                char swapped_name[MAX_FILENAME];
                strcpy(swapped_name, files[last].filename);
                files[i] = files[last];
                file_count--;
                // Update trie pointer for swapped element
                trie_delete(file_trie_root, swapped_name);
                trie_insert(file_trie_root, swapped_name, &files[i]);
                // re-check this index
            } else {
                file_count--;
            }
        } else {
            i++;
        }
    }

    // Invalidate search cache (pointers may have moved)
    cache_size = 0;

    // Persist immediately to avoid stale records after restart
    save_persistent_data();
}
