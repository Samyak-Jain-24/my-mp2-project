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

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <nm_port> <client_port> <storage_dir>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    
    nm_port = atoi(argv[1]);
    client_port = atoi(argv[2]);
    strcpy(storage_dir, argv[3]);
    
    // Create storage directory if it doesn't exist
    mkdir(storage_dir, 0755);
    
    // Get local IP
    strcpy(ss_ip, "127.0.0.1");  // For simplicity, using localhost
    
    printf("=== LangOS Storage Server ===\n");
    log_message("SS", "INFO", "Starting Storage Server on ports NM:%d Client:%d", nm_port, client_port);
    
    // Initialize file locks
    for (int i = 0; i < MAX_FILES; i++) {
        pthread_mutex_init(&file_locks[i], NULL);
        file_lock_info[i].lock_count = 0;
        file_lock_info[i].has_undo = 0;
    }
    
    // Register with Name Server
    register_with_nm();
    
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

void register_with_nm() {
    int sock = create_socket();
    if (sock < 0) {
        log_message("SS", "ERROR", "Failed to create socket for NM registration");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(PORT_NM);
    inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Failed to connect to Name Server");
        log_message("SS", "ERROR", "Failed to connect to Name Server");
        exit(EXIT_FAILURE);
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_REGISTER_SS;
    sprintf(msg.data, "%s %d %d", ss_ip, nm_port, client_port);
    
    send_message(sock, &msg);
    receive_message(sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        ss_id = atoi(msg.data);
        log_message("SS", "INFO", "Successfully registered with Name Server, assigned ID: %d", ss_id);
    } else {
        log_message("SS", "ERROR", "Failed to register with Name Server: %s", msg.error_msg);
        exit(EXIT_FAILURE);
    }
    
    close(sock);
}

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
    
    // Parse into sentences
    char (*sentences)[MAX_SENTENCE_LEN] = calloc(100, sizeof(char[MAX_SENTENCE_LEN]));
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
    
    // Check sentence index - allow writing to sentence_count (new sentence)
    if (msg->sentence_number < 0 || msg->sentence_number > sentence_count) {
        msg->error_code = ERR_INVALID_INDEX;
        sprintf(msg->error_msg, "Sentence index out of range (0-%d allowed)", sentence_count);
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }
    
    // If writing to a new sentence (sentence_count), add it
    if (msg->sentence_number == sentence_count) {
        sentence_count++;
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
    
    // Apply write operations from msg->data
    // Format: "word_index content\nword_index content\n..."
    
    // Make a copy of msg->data since strtok modifies the string
    char* data_copy = strdup(msg->data);
    if (data_copy == NULL) {
        msg->error_code = ERR_SERVER_ERROR;
        strcpy(msg->error_msg, "Memory allocation failed");
        free(sentences);
        pthread_mutex_unlock(&file_locks[lock_index]);
        return;
    }
    
    // Process each line
    char* saveptr1 = NULL;
    char* line = strtok_r(data_copy, "\n", &saveptr1);
    
    while (line != NULL) {
        int word_index;
        char word_content[MAX_CONTENT];
        
        // Parse line: word_index content
        if (sscanf(line, "%d ", &word_index) == 1) {
            // Extract content after the number
            char* content_start = line;
            while (*content_start && (*content_start == ' ' || (*content_start >= '0' && *content_start <= '9'))) {
                content_start++;
            }
            
            if (*content_start) {
                strncpy(word_content, content_start, sizeof(word_content) - 1);
                word_content[sizeof(word_content) - 1] = '\0';
                
                log_message("SS", "INFO", "Inserting at word %d: %s", word_index, word_content);
                
                // Allocate arrays on heap
                char (*words)[MAX_WORD_LEN] = calloc(500, sizeof(char[MAX_WORD_LEN]));
                if (words == NULL) {
                    msg->error_code = ERR_SERVER_ERROR;
                    strcpy(msg->error_msg, "Memory allocation failed");
                    free(data_copy);
                    free(sentences);
                    pthread_mutex_unlock(&file_locks[lock_index]);
                    return;
                }
                
                int word_count = 0;
                
                // Parse existing sentence
                if (msg->sentence_number < sentence_count && sentences[msg->sentence_number][0] != '\0') {
                    char* sentence_copy = strdup(sentences[msg->sentence_number]);
                    if (sentence_copy == NULL) {
                        msg->error_code = ERR_SERVER_ERROR;
                        strcpy(msg->error_msg, "Memory allocation failed");
                        free(words);
                        free(data_copy);
                        free(sentences);
                        pthread_mutex_unlock(&file_locks[lock_index]);
                        return;
                    }
                    
                    char* saveptr2 = NULL;
                    char* word = strtok_r(sentence_copy, " ", &saveptr2);
                    while (word != NULL && word_count < 500) {
                        strncpy(words[word_count], word, MAX_WORD_LEN - 1);
                        words[word_count][MAX_WORD_LEN - 1] = '\0';
                        word_count++;
                        word = strtok_r(NULL, " ", &saveptr2);
                    }
                    free(sentence_copy);
                }
                
                log_message("SS", "INFO", "Current word count: %d", word_count);
                
                // Check word index (1-based)
                if (word_index < 1 || word_index > word_count + 1) {
                    msg->error_code = ERR_INVALID_INDEX;
                    sprintf(msg->error_msg, "Word index out of range (1-%d allowed)", word_count + 1);
                    free(words);
                    free(data_copy);
                    free(sentences);
                    pthread_mutex_unlock(&file_locks[lock_index]);
                    return;
                }
                
                // Convert to 0-based
                word_index = word_index - 1;
                
                // Parse new content into words to insert
                char* content_copy = strdup(word_content);
                if (content_copy == NULL) {
                    msg->error_code = ERR_SERVER_ERROR;
                    strcpy(msg->error_msg, "Memory allocation failed");
                    free(words);
                    free(data_copy);
                    free(sentences);
                    pthread_mutex_unlock(&file_locks[lock_index]);
                    return;
                }
                
                char (*temp_words)[MAX_WORD_LEN] = calloc(100, sizeof(char[MAX_WORD_LEN]));
                if (temp_words == NULL) {
                    msg->error_code = ERR_SERVER_ERROR;
                    strcpy(msg->error_msg, "Memory allocation failed");
                    free(content_copy);
                    free(words);
                    free(data_copy);
                    free(sentences);
                    pthread_mutex_unlock(&file_locks[lock_index]);
                    return;
                }
                
                int temp_count = 0;
                char* saveptr3 = NULL;
                char* new_word = strtok_r(content_copy, " ", &saveptr3);
                while (new_word != NULL && temp_count < 100) {
                    strncpy(temp_words[temp_count], new_word, MAX_WORD_LEN - 1);
                    temp_words[temp_count][MAX_WORD_LEN - 1] = '\0';
                    temp_count++;
                    new_word = strtok_r(NULL, " ", &saveptr3);
                }
                
                free(content_copy);
                
                if (temp_count == 0) {
                    free(temp_words);
                    free(words);
                    line = strtok_r(NULL, "\n", &saveptr1);
                    continue;
                }
                
                log_message("SS", "INFO", "Inserting %d words at position %d", temp_count, word_index);
                
                // Shift existing words to make room
                for (int i = word_count - 1; i >= word_index; i--) {
                    if (i + temp_count < 500) {
                        strcpy(words[i + temp_count], words[i]);
                    }
                }
                
                // Insert new words
                for (int i = 0; i < temp_count && word_index + i < 500; i++) {
                    strcpy(words[word_index + i], temp_words[i]);
                }
                
                word_count += temp_count;
                if (word_count > 500) word_count = 500;
                
                // Reconstruct sentence
                sentences[msg->sentence_number][0] = '\0';
                for (int i = 0; i < word_count; i++) {
                    size_t current_len = strlen(sentences[msg->sentence_number]);
                    size_t word_len = strlen(words[i]);
                    
                    if (current_len + word_len + 2 < MAX_SENTENCE_LEN) {
                        strcat(sentences[msg->sentence_number], words[i]);
                        if (i < word_count - 1) {
                            strcat(sentences[msg->sentence_number], " ");
                        }
                    }
                }
                
                log_message("SS", "INFO", "Reconstructed sentence: %s", sentences[msg->sentence_number]);
                
                free(temp_words);
                free(words);
            }
        }
        
        line = strtok_r(NULL, "\n", &saveptr1);
    }
    
    free(data_copy);
    
    // Reconstruct file content
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
    for (int i = 0; i < file_lock_count; i++) {
        if (strcmp(file_lock_info[i].filename, filename) == 0) {
            return i;
        }
    }
    return -1;
}

void save_file_content(const char* filename, const char* content) {
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "%s%s", storage_dir, filename);
    
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
