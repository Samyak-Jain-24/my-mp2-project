#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../include/common.h"
#include "../../include/protocol.h"
#include "client_ops.h"

static void print_resp(const char* tag, const char* data, uint32_t len){
    printf("%s: %.*s\n", tag, (int)len, data);
}

static char* with_txt_if_missing(const char* fname) {
    if (strchr(fname, '.')) return strdup(fname);
    size_t n = strlen(fname);
    char* out = (char*)malloc(n + 5); // ".txt" + NUL
    strcpy(out, fname);
    strcat(out, ".txt");
    return out;
}

void handle_view(int nm_sock, int all_flag, int long_flag) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "VIEW %s %s", all_flag?"-a":"", long_flag?"-l":"");
    send_u32_and_data(nm_sock, cmd, (uint32_t)strlen(cmd));
    char* resp=NULL; uint32_t rlen=0;
    if (recv_u32_and_data(nm_sock, &resp, &rlen)==0) { print_resp("VIEW", resp, rlen); free(resp);} else { puts("VIEW: no response"); }
}

void handle_read(int nm_sock, const char* filename) {
    char* fname = with_txt_if_missing(filename);
    char cmd[256]; snprintf(cmd, sizeof(cmd), "LOCATE %s", fname);
    send_u32_and_data(nm_sock, cmd, (uint32_t)strlen(cmd));
    char* resp=NULL; uint32_t rlen=0;
    if (recv_u32_and_data(nm_sock, &resp, &rlen) < 0) { puts("READ: no response from NM"); free(fname); return; }
    char* line = strndup(resp, rlen);
    free(resp);
    if (!line) { puts("READ: alloc failed"); free(fname); return; }
    if (strncmp(line, "OK", 2) != 0) { printf("READ NM error: %s\n", line); free(line); free(fname); return; }
    char* save=NULL; (void)strtok_r(line, " \t\n", &save);
    char* ip = strtok_r(NULL, " \t\n", &save);
    char* port_s = strtok_r(NULL, " \t\n", &save);
    if (!ip || !port_s) { printf("READ: bad NM reply: %s\n", line); free(line); free(fname); return; }
    int port = atoi(port_s);
    int ss_sock = create_client_socket(ip, port);
    if (ss_sock < 0) { printf("READ: could not connect to SS %s:%d\n", ip, port); free(line); free(fname); return; }
    char rcmd[256]; snprintf(rcmd, sizeof(rcmd), "READ %s", fname);
    send_u32_and_data(ss_sock, rcmd, (uint32_t)strlen(rcmd));
    char* body=NULL; uint32_t blen=0;
    if (recv_u32_and_data(ss_sock, &body, &blen) < 0) { puts("READ: SS no response"); close(ss_sock); free(line); free(fname); return; }
    printf("%.*s\n", (int)blen, body);
    free(body);
    close(ss_sock);
    free(line);
    free(fname);
}

void handle_create(int nm_sock, const char* filename) {
    char* fname = with_txt_if_missing(filename);
    char cmd[256]; snprintf(cmd, sizeof(cmd), "CREATE %s %s", fname, "owner");
    send_u32_and_data(nm_sock, cmd, (uint32_t)strlen(cmd));
    char* resp=NULL; uint32_t rlen=0; if (recv_u32_and_data(nm_sock, &resp, &rlen)==0){ print_resp("CREATE", resp, rlen); free(resp);} else { puts("CREATE: no response"); }
    free(fname);
}

void handle_write(int nm_sock, const char* filename, int sentence) {
    if (sentence <= 0) { puts("WRITE: sentence index must be >= 1"); return; }
    char* fname = with_txt_if_missing(filename);
    char cmd[256]; snprintf(cmd, sizeof(cmd), "LOCATE %s", fname);
    send_u32_and_data(nm_sock, cmd, (uint32_t)strlen(cmd));
    char* resp=NULL; uint32_t rlen=0;
    if (recv_u32_and_data(nm_sock, &resp, &rlen) < 0) { puts("WRITE: no response from NM"); free(fname); return; }
    char* line = strndup(resp, rlen); free(resp);
    if (!line) { puts("WRITE: alloc failed"); free(fname); return; }
    if (strncmp(line, "OK", 2) != 0) { printf("WRITE NM error: %s\n", line); free(line); free(fname); return; }
    char* save=NULL; (void)strtok_r(line, " \t\n", &save);
    char* ip = strtok_r(NULL, " \t\n", &save); char* port_s = strtok_r(NULL, " \t\n", &save);
    if (!ip || !port_s) { printf("WRITE: bad NM reply: %s\n", line); free(line); free(fname); return; }
    int port = atoi(port_s);
    int ss_sock = create_client_socket(ip, port);
    if (ss_sock < 0) { printf("WRITE: could not connect to SS %s:%d\n", ip, port); free(line); free(fname); return; }
    char bcmd[256]; snprintf(bcmd, sizeof(bcmd), "WRITE_BEGIN %s %d", fname, sentence);
    send_u32_and_data(ss_sock, bcmd, (uint32_t)strlen(bcmd));
    char* bresp=NULL; uint32_t blen=0;
    if (recv_u32_and_data(ss_sock, &bresp, &blen) < 0) { puts("WRITE: SS no response on BEGIN"); close(ss_sock); free(line); free(fname); return; }
    if (strncmp(bresp, "OK", 2) != 0) { printf("WRITE BEGIN error: %.*s\n", (int)blen, bresp); free(bresp); close(ss_sock); free(line); free(fname); return; }
    free(bresp);
    printf("Enter new sentence (single line): "); fflush(stdout);
    char buf[2048]; if (!fgets(buf, sizeof(buf), stdin)) { puts("WRITE: input cancelled"); close(ss_sock); free(line); free(fname); return; }
    size_t n=strlen(buf); if (n && buf[n-1]=='\n') buf[n-1]='\0';
    size_t setlen = strlen(buf) + 11; char* setmsg = (char*)malloc(setlen);
    snprintf(setmsg, setlen, "WRITE_SET %s", buf);
    send_u32_and_data(ss_sock, setmsg, (uint32_t)strlen(setmsg));
    free(setmsg);
    char* sresp=NULL; uint32_t slen=0;
    if (recv_u32_and_data(ss_sock, &sresp, &slen) < 0) { puts("WRITE: SS no response on SET"); close(ss_sock); free(line); free(fname); return; }
    if (strncmp(sresp, "OK", 2) != 0) { printf("WRITE SET error: %.*s\n", (int)slen, sresp); free(sresp); close(ss_sock); free(line); free(fname); return; }
    free(sresp);
    const char* cmsg = "WRITE_COMMIT";
    send_u32_and_data(ss_sock, cmsg, (uint32_t)strlen(cmsg));
    char* cresp=NULL; uint32_t clen=0;
    if (recv_u32_and_data(ss_sock, &cresp, &clen) < 0) { puts("WRITE: SS no response on COMMIT"); close(ss_sock); free(line); free(fname); return; }
    printf("%.*s\n", (int)clen, cresp);
    free(cresp);
    close(ss_sock);
    free(line);
    free(fname);
}

void handle_undo(int nm_sock, const char* filename) { (void)nm_sock; (void)filename; puts("UNDO not implemented yet"); }
void handle_info(int nm_sock, const char* filename) {
    char* fname = with_txt_if_missing(filename);
    char cmd[256]; snprintf(cmd, sizeof(cmd), "INFO %s", fname);
    send_u32_and_data(nm_sock, cmd, (uint32_t)strlen(cmd));
    char* resp=NULL; uint32_t rlen=0; if (recv_u32_and_data(nm_sock, &resp, &rlen)==0){ print_resp("INFO", resp, rlen); free(resp);} else { puts("INFO: no response"); }
    free(fname);
}
void handle_delete(int nm_sock, const char* filename) { (void)nm_sock; (void)filename; puts("DELETE not implemented yet"); }
void handle_stream(int nm_sock, const char* filename) { (void)nm_sock; (void)filename; puts("STREAM not implemented yet"); }
void handle_list(int nm_sock) { (void)nm_sock; puts("LIST not implemented yet"); }
void handle_addaccess(int nm_sock, int write_mode, const char* filename, const char* user) { (void)nm_sock; (void)write_mode; (void)filename; (void)user; puts("ADDACCESS not implemented yet"); }
void handle_remaccess(int nm_sock, const char* filename, const char* user) { (void)nm_sock; (void)filename; (void)user; puts("REMACCESS not implemented yet"); }
void handle_exec(int nm_sock, const char* filename) { (void)nm_sock; (void)filename; puts("EXEC not implemented yet"); }
