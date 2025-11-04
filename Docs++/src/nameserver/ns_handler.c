#include <stdio.h>
#include "ns_handler.h"

void* client_handler(void* arg) {
    (void)arg; printf("client_handler stub\n"); return NULL;
}
void* ss_handler(void* arg) {
    (void)arg; printf("ss_handler stub\n"); return NULL;
}
