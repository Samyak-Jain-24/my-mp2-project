#ifndef CLIENT_OPS_H
#define CLIENT_OPS_H

void handle_view(int sock, int all_flag, int long_flag);
void handle_read(int nm_sock, const char* filename);
void handle_create(int nm_sock, const char* filename);
void handle_write(int nm_sock, const char* filename, int sentence);
void handle_undo(int nm_sock, const char* filename);
void handle_info(int nm_sock, const char* filename);
void handle_delete(int nm_sock, const char* filename);
void handle_stream(int nm_sock, const char* filename);
void handle_list(int nm_sock);
void handle_addaccess(int nm_sock, int write_mode, const char* filename, const char* user);
void handle_remaccess(int nm_sock, const char* filename, const char* user);
void handle_exec(int nm_sock, const char* filename);

#endif // CLIENT_OPS_H
