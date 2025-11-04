#include <stdio.h>
#include "ss_handler.h"

void* ss_handle_nm(void* arg) { (void)arg; printf("ss_handle_nm stub\n"); return NULL; }
void* ss_handle_client(void* arg) { (void)arg; printf("ss_handle_client stub\n"); return NULL; }
