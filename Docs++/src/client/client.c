#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <signal.h>
#include "../../include/common.h"
#include "client_ops.h"

static void trim_newline(char* s){
    size_t n=strlen(s); if(n&&s[n-1]=='\n') s[n-1]='\0';
}

static void print_help(){
    printf("Commands:\n");
    printf("  VIEW [-a] [-l]\n");
    printf("  READ <filename>\n");
    printf("  CREATE <filename>\n");
    printf("  WRITE <filename> <sentence_number>\n");
    printf("  UNDO <filename>\n");
    printf("  INFO <filename>\n");
    printf("  DELETE <filename>\n");
    printf("  STREAM <filename>\n");
    printf("  LIST\n");
    printf("  ADDACCESS -R|-W <filename> <user>\n");
    printf("  REMACCESS <filename> <user>\n");
    printf("  EXEC <filename>\n");
    printf("  HELP\n");
    printf("  QUIT\n");
}

int main() {
    signal(SIGPIPE, SIG_IGN); // avoid client exit on broken pipe
    const char* nm_ip = getenv("NM_IP"); if (!nm_ip) nm_ip = "127.0.0.1";
    const char* nm_port_s = getenv("NM_PORT"); int nm_port = nm_port_s?atoi(nm_port_s):9000;
    printf("Client (C). Connecting to NameServer %s:%d...\n", nm_ip, nm_port);
    int nm_sock = create_client_socket(nm_ip, nm_port);
    if (nm_sock < 0) { fprintf(stderr, "Failed to connect to NameServer\n"); return 1; }

    char line[1024];
    print_help();
    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        trim_newline(line);
        if (!*line) continue;
        // tokenize
        char* argv[8]; int argc=0; char* tok = strtok(line, " \t");
        while (tok && argc < 8) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
        if (argc==0) continue;
        for(int i=0;i<strlen(argv[0]);++i) argv[0][i]=toupper((unsigned char)argv[0][i]);
        if (strcmp(argv[0], "QUIT")==0) {
            break;
        } else if (strcmp(argv[0], "HELP")==0) {
            print_help();
        } else if (strcmp(argv[0], "VIEW")==0) {
            int all_flag=0,long_flag=0;
            for (int i=1;i<argc;i++) {
                if (strcmp(argv[i], "-a")==0) all_flag=1; else if (strcmp(argv[i], "-l")==0) long_flag=1;
            }
            handle_view(nm_sock, all_flag, long_flag);
        } else if (strcmp(argv[0], "READ")==0 && argc==2) {
            handle_read(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "CREATE")==0 && argc==2) {
            handle_create(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "WRITE")==0 && argc==3) {
            int sentence = atoi(argv[2]);
            handle_write(nm_sock, argv[1], sentence);
        } else if (strcmp(argv[0], "UNDO")==0 && argc==2) {
            handle_undo(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "INFO")==0 && argc==2) {
            handle_info(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "DELETE")==0 && argc==2) {
            handle_delete(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "STREAM")==0 && argc==2) {
            handle_stream(nm_sock, argv[1]);
        } else if (strcmp(argv[0], "LIST")==0) {
            handle_list(nm_sock);
        } else if (strcmp(argv[0], "ADDACCESS")==0 && argc==4) {
            int write_mode = (strcmp(argv[1], "-W")==0);
            handle_addaccess(nm_sock, write_mode, argv[2], argv[3]);
        } else if (strcmp(argv[0], "REMACCESS")==0 && argc==3) {
            handle_remaccess(nm_sock, argv[1], argv[2]);
        } else if (strcmp(argv[0], "EXEC")==0 && argc==2) {
            handle_exec(nm_sock, argv[1]);
        } else {
            printf("Unknown/invalid command. Type HELP.\n");
        }
    }
    printf("Goodbye.\n");
    return 0;
}
