#include "common.h"

// Global variables
char username[MAX_USERNAME];
int nm_socket = -1;

// Function prototypes
void connect_to_nm();
void register_with_nm();
void handle_view_command(char* command);
void handle_read_command(char* command);
void handle_create_command(char* command);
void handle_write_command(char* command);
void handle_delete_command(char* command);
void handle_info_command(char* command);
void handle_stream_command(char* command);
void handle_list_command();
void handle_addaccess_command(char* command);
void handle_remaccess_command(char* command);
void handle_exec_command(char* command);
void handle_undo_command(char* command);
int connect_to_ss(const char* ss_ip, int ss_port);

int main() {
    printf("=== LangOS Distributed File System - Client ===\n");
    
    // Get username
    printf("Enter your username: ");
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "Failed to read username\n");
        exit(EXIT_FAILURE);
    }
    username[strcspn(username, "\n")] = 0;  // Remove newline
    
    printf("Welcome, %s!\n", username);
    log_message("CLIENT", "INFO", "User %s logged in", username);
    
    // Connect and register with Name Server
    connect_to_nm();
    register_with_nm();
    
    printf("\nAvailable commands:\n");
    printf("  VIEW [-a] [-l] [-al]\n");
    printf("  READ <filename>\n");
    printf("  CREATE <filename>\n");
    printf("  WRITE <filename> <sentence_number>\n");
    printf("  DELETE <filename>\n");
    printf("  INFO <filename>\n");
    printf("  STREAM <filename>\n");
    printf("  LIST\n");
    printf("  ADDACCESS -R/-W <filename> <username>\n");
    printf("  REMACCESS <filename> <username>\n");
    printf("  EXEC <filename>\n");
    printf("  UNDO <filename>\n");
    printf("  EXIT\n\n");
    
    // Command loop
    char command[MAX_COMMAND];
    while (1) {
        printf("%s> ", username);
        fflush(stdout);
        
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        
        command[strcspn(command, "\n")] = 0;  // Remove newline
        
        if (strlen(command) == 0) {
            continue;
        }
        
        // Parse and route command
        if (strncmp(command, "VIEW", 4) == 0) {
            handle_view_command(command);
        } else if (strncmp(command, "READ", 4) == 0) {
            handle_read_command(command);
        } else if (strncmp(command, "CREATE", 6) == 0) {
            handle_create_command(command);
        } else if (strncmp(command, "WRITE", 5) == 0) {
            handle_write_command(command);
        } else if (strncmp(command, "DELETE", 6) == 0) {
            handle_delete_command(command);
        } else if (strncmp(command, "INFO", 4) == 0) {
            handle_info_command(command);
        } else if (strncmp(command, "STREAM", 6) == 0) {
            handle_stream_command(command);
        } else if (strncmp(command, "LIST", 4) == 0) {
            handle_list_command();
        } else if (strncmp(command, "ADDACCESS", 9) == 0) {
            handle_addaccess_command(command);
        } else if (strncmp(command, "REMACCESS", 9) == 0) {
            handle_remaccess_command(command);
        } else if (strncmp(command, "EXEC", 4) == 0) {
            handle_exec_command(command);
        } else if (strncmp(command, "UNDO", 4) == 0) {
            handle_undo_command(command);
        } else if (strncmp(command, "EXIT", 4) == 0 || strncmp(command, "exit", 4) == 0) {
            printf("Goodbye!\n");
            break;
        } else {
            printf("Unknown command. Type 'EXIT' to quit.\n");
        }
    }
    
    if (nm_socket >= 0) {
        close(nm_socket);
    }
    
    return 0;
}

void connect_to_nm() {
    nm_socket = create_socket();
    if (nm_socket < 0) {
        fprintf(stderr, "Failed to create socket\n");
        exit(EXIT_FAILURE);
    }
    
    struct sockaddr_in nm_addr;
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(PORT_NM);
    inet_pton(AF_INET, "127.0.0.1", &nm_addr.sin_addr);
    
    if (connect(nm_socket, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {
        perror("Failed to connect to Name Server");
        fprintf(stderr, "Make sure the Name Server is running.\n");
        exit(EXIT_FAILURE);
    }
    
    printf("Connected to Name Server.\n");
}

void register_with_nm() {
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_REGISTER_CLIENT;
    strcpy(msg.username, username);
    sprintf(msg.data, "127.0.0.1 %d %d", PORT_CLIENT_BASE, PORT_CLIENT_BASE + 1);
    
    if (send_message(nm_socket, &msg) < 0) {
        fprintf(stderr, "Failed to send registration\n");
        exit(EXIT_FAILURE);
    }
    
    if (receive_message(nm_socket, &msg) < 0) {
        fprintf(stderr, "Failed to receive registration response\n");
        exit(EXIT_FAILURE);
    }
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("Successfully registered with Name Server.\n");
    } else {
        fprintf(stderr, "Registration failed: %s\n", msg.error_msg);
        exit(EXIT_FAILURE);
    }
}

void handle_view_command(char* command) {
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_VIEW;
    strcpy(msg.username, username);
    msg.flags = 0;
    
    // Parse flags
    if (strstr(command, "-a")) msg.flags |= 1;
    if (strstr(command, "-l")) msg.flags |= 2;
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s", msg.data);
    } else {
        print_error(msg.error_code, "VIEW");
    }
}

void handle_read_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "READ %s", filename) != 1) {
        printf("Usage: READ <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_READ;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "READ");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        return;
    }
    
    // Connect to storage server
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    sscanf(msg.data, "%s %d", ss_ip, &ss_port);
    
    int ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server\n");
        return;
    }
    
    // Request file content
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_READ;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s\n", msg.data);
    } else {
        print_error(msg.error_code, "READ");
    }
    
    close(ss_sock);
}

void handle_create_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "CREATE %s", filename) != 1) {
        printf("Usage: CREATE <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_CREATE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("File Created Successfully!\n");
    } else {
        print_error(msg.error_code, "CREATE");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
}

void handle_write_command(char* command) {
    char filename[MAX_FILENAME];
    int sentence_number;
    
    if (sscanf(command, "WRITE %s %d", filename, &sentence_number) != 2) {
        printf("Usage: WRITE <filename> <sentence_number>\n");
        return;
    }
    
    // Get SS connection info from NM
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_WRITE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    msg.sentence_number = sentence_number;
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "WRITE");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        return;
    }
    
    // Connect to storage server
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    sscanf(msg.data, "%s %d", ss_ip, &ss_port);
    
    // Phase 1: Lock the sentence
    int ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server\n");
        return;
    }
    
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_LOCK_SENTENCE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    msg.sentence_number = sentence_number;
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    close(ss_sock);  // Close after lock operation
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "LOCK");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        return;
    }
    
    printf("Sentence %d locked successfully!\n", sentence_number);
    
    // Phase 2: Collect write operations
    char write_data[MAX_CONTENT] = "";
    char line[MAX_COMMAND];
    
    printf("Enter write commands (format: <word_index> <content>)\n");
    printf("Type ETIRW when done:\n");
    
    while (1) {
        printf("Client: ");
        fflush(stdout);
        
        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }
        
        line[strcspn(line, "\n")] = 0;
        
        if (strcmp(line, "ETIRW") == 0) {
            break;
        }
        
        strcat(write_data, line);
        strcat(write_data, "\n");
    }
    
    // Phase 3: Send write request - reconnect to storage server
    ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server for write\n");
        // Try to unlock anyway
        ss_sock = connect_to_ss(ss_ip, ss_port);
        if (ss_sock >= 0) {
            memset(&msg, 0, sizeof(Message));
            msg.op_code = OP_UNLOCK_SENTENCE;
            strcpy(msg.username, username);
            strcpy(msg.filename, filename);
            msg.sentence_number = sentence_number;
            send_message(ss_sock, &msg);
            receive_message(ss_sock, &msg);
            close(ss_sock);
        }
        return;
    }
    
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_WRITE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    msg.sentence_number = sentence_number;
    strcpy(msg.data, write_data);
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    close(ss_sock);  // Close after write operation
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "WRITE");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        // Still need to unlock even if write failed
    } else {
        printf("Write Successful!\n");
    }
    
    // Phase 4: Unlock the sentence - reconnect to storage server
    ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server for unlock\n");
        return;
    }
    
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_UNLOCK_SENTENCE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    msg.sentence_number = sentence_number;
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("Sentence unlocked!\n");
    }
    
    close(ss_sock);  // Close after unlock operation
}

void handle_delete_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "DELETE %s", filename) != 1) {
        printf("Usage: DELETE <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_DELETE;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("File '%s' deleted successfully!\n", filename);
    } else {
        print_error(msg.error_code, "DELETE");
    }
}

void handle_info_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "INFO %s", filename) != 1) {
        printf("Usage: INFO <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_INFO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s\n", msg.data);
    } else {
        print_error(msg.error_code, "INFO");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
}

void handle_stream_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "STREAM %s", filename) != 1) {
        printf("Usage: STREAM <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_STREAM;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "STREAM");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        return;
    }
    
    // Connect to storage server
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    sscanf(msg.data, "%s %d", ss_ip, &ss_port);
    
    int ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server\n");
        return;
    }
    
    // Request stream
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_STREAM;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "STREAM");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        close(ss_sock);
        return;
    }
    
    // Receive words
    while (1) {
        memset(&msg, 0, sizeof(Message));
        if (receive_message(ss_sock, &msg) <= 0) {
            fprintf(stderr, "\nERROR: Storage server disconnected\n");
            break;
        }
        
        if (strcmp(msg.data, "STOP") == 0) {
            printf("\n");
            break;
        }
        
        printf("%s ", msg.data);
        fflush(stdout);
    }
    
    close(ss_sock);
}

void handle_list_command() {
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_LIST;
    strcpy(msg.username, username);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s", msg.data);
    } else {
        print_error(msg.error_code, "LIST");
    }
}

void handle_addaccess_command(char* command) {
    char flag[8], filename[MAX_FILENAME], target_user[MAX_USERNAME];
    
    if (sscanf(command, "ADDACCESS %s %s %s", flag, filename, target_user) != 3) {
        printf("Usage: ADDACCESS -R/-W <filename> <username>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_ADDACCESS;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    strcpy(msg.data, target_user);
    msg.flags = (strcmp(flag, "-W") == 0) ? 1 : 0;
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("Access granted successfully!\n");
    } else {
        print_error(msg.error_code, "ADDACCESS");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
}

void handle_remaccess_command(char* command) {
    char filename[MAX_FILENAME], target_user[MAX_USERNAME];
    
    if (sscanf(command, "REMACCESS %s %s", filename, target_user) != 2) {
        printf("Usage: REMACCESS <filename> <username>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_REMACCESS;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    strcpy(msg.data, target_user);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("Access removed successfully!\n");
    } else {
        print_error(msg.error_code, "REMACCESS");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
}

void handle_exec_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "EXEC %s", filename) != 1) {
        printf("Usage: EXEC <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_EXEC;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("%s", msg.data);
    } else {
        print_error(msg.error_code, "EXEC");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
}

void handle_undo_command(char* command) {
    char filename[MAX_FILENAME];
    if (sscanf(command, "UNDO %s", filename) != 1) {
        printf("Usage: UNDO <filename>\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    
    msg.op_code = OP_UNDO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    receive_message(nm_socket, &msg);
    
    if (msg.error_code != ERR_SUCCESS) {
        print_error(msg.error_code, "UNDO");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
        return;
    }
    
    // Connect to storage server
    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;
    sscanf(msg.data, "%s %d", ss_ip, &ss_port);
    
    int ss_sock = connect_to_ss(ss_ip, ss_port);
    if (ss_sock < 0) {
        fprintf(stderr, "Failed to connect to storage server\n");
        return;
    }
    
    // Request undo
    memset(&msg, 0, sizeof(Message));
    msg.op_code = OP_UNDO;
    strcpy(msg.username, username);
    strcpy(msg.filename, filename);
    
    send_message(ss_sock, &msg);
    receive_message(ss_sock, &msg);
    
    if (msg.error_code == ERR_SUCCESS) {
        printf("Undo Successful!\n");
    } else {
        print_error(msg.error_code, "UNDO");
        if (msg.error_msg[0] != '\0') {
            fprintf(stderr, "Details: %s\n", msg.error_msg);
        }
    }
    
    close(ss_sock);
}

int connect_to_ss(const char* ss_ip, int ss_port) {
    int sock = create_socket();
    if (sock < 0) {
        return -1;
    }
    
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}
