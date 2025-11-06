#include "common.h"
#include <stdarg.h>

// Get current timestamp
char* get_timestamp() {
    static char buffer[64];
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", t);
    return buffer;
}

// Logging function
void log_message(const char* component, const char* level, const char* format, ...) {
    char log_filename[128];
    snprintf(log_filename, sizeof(log_filename), "%s.log", component);
    
    FILE* log_file = fopen(log_filename, "a");
    if (log_file == NULL) {
        perror("Failed to open log file");
        return;
    }
    
    fprintf(log_file, "[%s] [%s] ", get_timestamp(), level);
    printf("[%s] [%s] ", get_timestamp(), level);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    printf("\n");
    
    fclose(log_file);
}

// Log request
void log_request(const char* component, const char* client_ip, int port, const char* username, const char* operation) {
    log_message(component, "REQUEST", "From %s:%d [%s] - %s", client_ip, port, username, operation);
}

// Log response
void log_response(const char* component, const char* client_ip, int port, int status, const char* message) {
    log_message(component, "RESPONSE", "To %s:%d - Status: %d, %s", client_ip, port, status, message);
}

// Trim whitespace from string
void trim_whitespace(char* str) {
    if (str == NULL) return;
    
    char* end;
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;
    
    if (*str == 0) return;
    
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    
    end[1] = '\0';
    
    memmove(str - (str - str), str, strlen(str) + 1);
}

// Create socket
int create_socket() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return -1;
    }
    
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        close(sock);
        return -1;
    }
    
    return sock;
}

// Send message
int send_message(int socket_fd, Message* msg) {
    int total_sent = 0;
    int bytes_to_send = sizeof(Message);
    char* ptr = (char*)msg;
    
    while (total_sent < bytes_to_send) {
        int sent = send(socket_fd, ptr + total_sent, bytes_to_send - total_sent, 0);
        if (sent <= 0) {
            if (sent < 0) perror("Send failed");
            return -1;
        }
        total_sent += sent;
    }
    
    return total_sent;
}

// Receive message
int receive_message(int socket_fd, Message* msg) {
    int total_received = 0;
    int bytes_to_receive = sizeof(Message);
    char* ptr = (char*)msg;
    
    while (total_received < bytes_to_receive) {
        int received = recv(socket_fd, ptr + total_received, bytes_to_receive - total_received, 0);
        if (received <= 0) {
            if (received < 0) perror("Receive failed");
            return -1;
        }
        total_received += received;
    }
    
    return total_received;
}

// Print error
void print_error(int error_code, const char* context) {
    const char* error_messages[] = {
        "Success",
        "File not found",
        "File already exists",
        "Access denied",
        "Sentence is locked by another user",
        "Invalid index",
        "Server error",
        "Connection failed",
        "Invalid command",
        "Not the owner",
        "User not found",
        "Storage server not found",
        "No undo history available"
    };
    
    if (error_code >= 0 && error_code <= ERR_NO_UNDO) {
        fprintf(stderr, "ERROR [%s]: %s\n", context, error_messages[error_code]);
    } else {
        fprintf(stderr, "ERROR [%s]: Unknown error code %d\n", context, error_code);
    }
}

// Check access
int check_access(FileMetadata* file, const char* username, int required_access) {
    if (file == NULL || username == NULL) return 0;
    
    // Owner always has full access
    if (strcmp(file->owner, username) == 0) return 1;
    
    // Check access list
    for (int i = 0; i < file->access_count; i++) {
        if (strcmp(file->access_list[i].username, username) == 0) {
            if (required_access == ACCESS_READ) {
                return file->access_list[i].access_type >= ACCESS_READ;
            } else if (required_access == ACCESS_WRITE) {
                return file->access_list[i].access_type >= ACCESS_WRITE;
            }
        }
    }
    
    return 0;
}

// Trie Operations
TrieNode* create_trie_node() {
    TrieNode* node = (TrieNode*)calloc(1, sizeof(TrieNode));
    if (node == NULL) {
        perror("Failed to allocate trie node");
        return NULL;
    }
    node->is_end = 0;
    node->file_info = NULL;
    for (int i = 0; i < 256; i++) {
        node->children[i] = NULL;
    }
    return node;
}

void trie_insert(TrieNode* root, const char* filename, FileMetadata* file_info) {
    if (root == NULL || filename == NULL) return;
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char index = (unsigned char)filename[i];
        if (current->children[index] == NULL) {
            current->children[index] = create_trie_node();
        }
        current = current->children[index];
    }
    
    current->is_end = 1;
    current->file_info = file_info;
}

FileMetadata* trie_search(TrieNode* root, const char* filename) {
    if (root == NULL || filename == NULL) return NULL;
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char index = (unsigned char)filename[i];
        if (current->children[index] == NULL) {
            return NULL;
        }
        current = current->children[index];
    }
    
    if (current->is_end) {
        return current->file_info;
    }
    
    return NULL;
}

void trie_delete(TrieNode* root, const char* filename) {
    if (root == NULL || filename == NULL) return;
    
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        unsigned char index = (unsigned char)filename[i];
        if (current->children[index] == NULL) {
            return;
        }
        current = current->children[index];
    }
    
    if (current->is_end) {
        current->is_end = 0;
        current->file_info = NULL;
    }
}

void trie_free(TrieNode* root) {
    if (root == NULL) return;
    
    for (int i = 0; i < 256; i++) {
        if (root->children[i] != NULL) {
            trie_free(root->children[i]);
        }
    }
    
    free(root);
}
