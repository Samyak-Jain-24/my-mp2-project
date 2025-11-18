#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>

// Constants
#define MAX_FILENAME 256
#define MAX_PATH 512
#define MAX_USERNAME 64
#define MAX_CONTENT 8192
#define MAX_COMMAND 1024
#define MAX_CLIENTS 100
#define MAX_SS 50
#define MAX_FILES 10000
#define MAX_SENTENCE_LEN 4096
#define MAX_WORD_LEN 256
#define MAX_ACCESS_LIST 50
#define BUFFER_SIZE 8192
#define PORT_NM 8080
#define PORT_SS_BASE 9000
#define PORT_CLIENT_BASE 10000

// Error Codes
#define ERR_SUCCESS 0
#define ERR_FILE_NOT_FOUND 1
#define ERR_FILE_EXISTS 2
#define ERR_ACCESS_DENIED 3
#define ERR_SENTENCE_LOCKED 4
#define ERR_INVALID_INDEX 5
#define ERR_SERVER_ERROR 6
#define ERR_CONNECTION_FAILED 7
#define ERR_INVALID_COMMAND 8
#define ERR_NOT_OWNER 9
#define ERR_USER_NOT_FOUND 10
#define ERR_SS_NOT_FOUND 11
#define ERR_NO_UNDO 12

// Operation Codes
#define OP_VIEW 1
#define OP_READ 2
#define OP_CREATE 3
#define OP_WRITE 4
#define OP_DELETE 5
#define OP_INFO 6
#define OP_STREAM 7
#define OP_LIST 8
#define OP_ADDACCESS 9
#define OP_REMACCESS 10
#define OP_EXEC 11
#define OP_UNDO 12
#define OP_LOCK_SENTENCE 13
#define OP_UNLOCK_SENTENCE 14
#define OP_REGISTER_SS 20
#define OP_REGISTER_CLIENT 21
#define OP_SS_ACK 22
// Bonus ops
#define OP_CREATEFOLDER 23
#define OP_MOVE 24
#define OP_VIEWFOLDER 25
#define OP_CHECKPOINT 26
#define OP_VIEWCHECKPOINT 27
#define OP_REVERT 28
#define OP_LISTCHECKPOINTS 29
#define OP_REQACCESS 30
#define OP_VIEWREQUESTS 31
#define OP_APPROVE 32
#define OP_DENY 33
#define OP_REPL_CREATE 34
#define OP_REPL_DELETE 35
#define OP_REPL_WRITE 36
#define OP_REPL_MOVE 37
#define OP_RECENTS 38
// Replication of folder creation
#define OP_REPL_CREATEFOLDER 39

// Access Types
#define ACCESS_NONE 0
#define ACCESS_READ 1
#define ACCESS_WRITE 2

// Structures
typedef struct {
    char username[MAX_USERNAME];
    int access_type; // ACCESS_READ or ACCESS_WRITE
} AccessEntry;

typedef struct {
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int ss_id;
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    // Replica info (for fault tolerance)
    int replica_ss_id;
    char replica_ss_ip[INET_ADDRSTRLEN];
    int replica_ss_port;
    AccessEntry access_list[MAX_ACCESS_LIST];
    int access_count;
    // Pending access requests (bonus)
    AccessEntry pending_requests[MAX_ACCESS_LIST];
    int pending_count;
    time_t created_time;
    time_t modified_time;
    time_t accessed_time;
    long size;
    int word_count;
    int char_count;
    char last_accessed_by[MAX_USERNAME];
} FileMetadata;

typedef struct {
    int ss_id;
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    int active;
    char files[MAX_FILES][MAX_FILENAME];
    int file_count;
} StorageServerInfo;

typedef struct {
    char username[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int ss_port;
    int socket_fd;
    int active;
} ClientInfo;

typedef struct {
    int op_code;
    char username[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char data[MAX_CONTENT];
    int sentence_number;
    int word_index;
    int flags; // For -a, -l, -R, -W and replication flags
    int error_code;
    char error_msg[256];
    int data_size;
} Message;

typedef struct {
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
} SSConnection;

// Trie Node for efficient file search
typedef struct TrieNode {
    struct TrieNode* children[256];
    int is_end;
    FileMetadata* file_info;
} TrieNode;

// Logging
void log_message(const char* component, const char* level, const char* format, ...);
void log_request(const char* component, const char* client_ip, int port, const char* username, const char* operation);
void log_response(const char* component, const char* client_ip, int port, int status, const char* message);

// Utility Functions
char* get_timestamp();
void trim_whitespace(char* str);
int create_socket();
int send_message(int socket_fd, Message* msg);
int receive_message(int socket_fd, Message* msg);
void print_error(int error_code, const char* context);
int check_access(FileMetadata* file, const char* username, int required_access);

// Trie Operations
TrieNode* create_trie_node();
void trie_insert(TrieNode* root, const char* filename, FileMetadata* file_info);
FileMetadata* trie_search(TrieNode* root, const char* filename);
void trie_delete(TrieNode* root, const char* filename);
void trie_free(TrieNode* root);

// Flags (bitmask)
#define FLAG_REPL 0x100

#endif // COMMON_H
