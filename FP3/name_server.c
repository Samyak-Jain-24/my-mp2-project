#include "common.h"

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
FileMetadata* search_file_cached(const char* filename);
void update_cache(const char* filename, FileMetadata* file_info);
void load_persistent_data();
void save_persistent_data();

// Helpers to validate SS state and purge stale metadata
static int ss_file_exists(FileMetadata* file);
static void purge_file_metadata(const char* filename);

int main() {
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
    
    // Accept connections
    while (1) {
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
    
    close(nm_socket);
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
            case OP_CREATE:
                handle_create_command(client_sock, &msg);
                break;
            case OP_DELETE:
                handle_delete_command(client_sock, &msg);
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
            case OP_EXEC:
                handle_exec_command(client_sock, &msg);
                break;
            case OP_READ:
            case OP_STREAM:
            case OP_UNDO:
                handle_read_stream_undo_command(client_sock, &msg);
                break;
            case OP_WRITE:
                handle_write_command(client_sock, &msg);
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
    
    if (ss_count >= MAX_SS) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Maximum storage servers reached");
        pthread_mutex_unlock(&nm_lock);
        send_message(socket_fd, msg);
        return;
    }
    
    StorageServerInfo* ss = &storage_servers[ss_count];
    ss->ss_id = ss_count;
    sscanf(msg->data, "%s %d %d", ss->ip, &ss->nm_port, &ss->client_port);
    ss->active = 1;
    ss->file_count = 0;
    
    log_message("NM", "INFO", "Registered Storage Server %d: %s:%d (client_port: %d)", 
                ss->ss_id, ss->ip, ss->nm_port, ss->client_port);
    
    ss_count++;
    
    msg->error_code = ERR_SUCCESS;
    sprintf(msg->data, "%d", ss->ss_id);
    
    pthread_mutex_unlock(&nm_lock);
    send_message(socket_fd, msg);
    save_persistent_data();
}

void register_client(int socket_fd, Message* msg) {
    pthread_mutex_lock(&nm_lock);

    // If a client with the same username already exists, update it instead of adding duplicates
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].username, msg->username) == 0) {
            sscanf(msg->data, "%s %d %d", clients[i].ip, &clients[i].nm_port, &clients[i].ss_port);
            clients[i].socket_fd = socket_fd;
            clients[i].active = 1;
            log_message("NM", "INFO", "Re-registered Client: %s from %s:%d", clients[i].username, clients[i].ip, clients[i].nm_port);
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

    log_message("NM", "INFO", "Registered Client: %s from %s:%d", client->username, client->ip, client->nm_port);

    client_count++;

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
        int exists = ss_file_exists(file);
        if (exists == 0) {
            purge_file_metadata(file->filename);
            // After purge, files[i] now contains a new entry (swapped), so re-check same index
            continue;
        }
        // If SS unreachable, we don't purge metadata; just skip listing to avoid showing ghost files
        if (exists < 0) {
            i++;
            continue;
        }
        
        // Check access
        if (!show_all && !check_access(file, msg->username, ACCESS_READ)) {
            i++;
            continue;
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
                snprintf(line, sizeof(line), "| %-10s | %5d | %5d | %16s | %-5s |\n",
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
        strcat(response, "---------------------------------------------------------\n");
    }
    
    strcpy(msg->data, response);
    msg->error_code = ERR_SUCCESS;
    
    pthread_mutex_unlock(&nm_lock);
    send_message(client_sock, msg);
    
    log_response("NM", "client", client_sock, ERR_SUCCESS, "VIEW command completed");
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
    
    int ss_index = file_count % ss_count;  // Simple round-robin
    StorageServerInfo* ss = &storage_servers[ss_index];
    
    // Create file metadata
    FileMetadata* file = &files[file_count];
    strcpy(file->filename, msg->filename);
    strcpy(file->owner, msg->username);
    file->ss_id = ss->ss_id;
    strcpy(file->ss_ip, ss->ip);
    file->ss_port = ss->client_port;
    file->access_count = 0;
    file->created_time = time(NULL);
    file->modified_time = time(NULL);
    file->accessed_time = time(NULL);
    file->size = 0;
    file->word_count = 0;
    file->char_count = 0;
    strcpy(file->last_accessed_by, msg->username);
    
    // Add to trie
    trie_insert(file_trie_root, msg->filename, file);
    
    // Add to SS file list
    strcpy(ss->files[ss->file_count], msg->filename);
    ss->file_count++;
    
    file_count++;
    
    // Forward to storage server
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
    
    send_message(ss_sock, msg);
    receive_message(ss_sock, msg);
    close(ss_sock);
    
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
    
    if (msg->op_code == OP_READ || msg->op_code == OP_STREAM) {
        if (!check_access(file, msg->username, ACCESS_READ)) {
            msg->error_code = ERR_ACCESS_DENIED;
            strcpy(msg->error_msg, "Access denied");
            pthread_mutex_unlock(&nm_lock);
            send_message(client_sock, msg);
            return;
        }
    }
    
    // Return SS connection info
    SSConnection ss_conn;
    strcpy(ss_conn.ss_ip, file->ss_ip);
    ss_conn.ss_port = file->ss_port;
    
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
    
    // Return SS connection info
    SSConnection ss_conn;
    strcpy(ss_conn.ss_ip, file->ss_ip);
    ss_conn.ss_port = file->ss_port;
    
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
    
    fread(&file_count, sizeof(int), 1, fp);
    fread(files, sizeof(FileMetadata), file_count, fp);
    fread(&ss_count, sizeof(int), 1, fp);
    fread(storage_servers, sizeof(StorageServerInfo), ss_count, fp);
    
    // Rebuild trie with de-duplication to recover from any stale entries
    int new_count = 0;
    for (int i = 0; i < file_count; i++) {
        int exists = 0;
        for (int j = 0; j < new_count; j++) {
            if (strcmp(files[j].filename, files[i].filename) == 0) { exists = 1; break; }
        }
        if (!exists) {
            if (i != new_count) {
                files[new_count] = files[i];
            }
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
    if (file->ss_id < 0 || file->ss_id >= ss_count) return -1;
    StorageServerInfo* ss = &storage_servers[file->ss_id];

    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0) return -1;

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss->nm_port);
    if (inet_pton(AF_INET, ss->ip, &ss_addr.sin_addr) != 1) {
        close(ss_sock);
        return -1;
    }
    if (connect(ss_sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        close(ss_sock);
        return -1;
    }

    Message m; memset(&m, 0, sizeof(m));
    m.op_code = OP_READ; // SS NM listener supports OP_READ
    strncpy(m.filename, file->filename, sizeof(m.filename)-1);
    strncpy(m.username, "NM", sizeof(m.username)-1);
    send_message(ss_sock, &m);
    receive_message(ss_sock, &m);
    close(ss_sock);

    if (m.error_code == ERR_SUCCESS) return 1;
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
