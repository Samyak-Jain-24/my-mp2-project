#include <stdlib.h>
#include <string.h>
#include "undo.h"

void undo_init(UndoStack* u) { u->stack=NULL; u->size=0; u->cap=0; }
void undo_push(UndoStack* u, const char* snapshot) {
    if (u->size==u->cap) { size_t n=u->cap?u->cap*2:8; u->stack=realloc(u->stack, n*sizeof(char*)); u->cap=n; }
    u->stack[u->size++] = strdup(snapshot);
}
char* undo_pop(UndoStack* u) { if (!u->size) return NULL; return u->stack[--u->size]; }
void undo_free(UndoStack* u) { for(size_t i=0;i<u->size;i++) free(u->stack[i]); free(u->stack); }
