#ifndef UNDO_H
#define UNDO_H

#include <stddef.h>

typedef struct { char** stack; size_t size; size_t cap; } UndoStack;

void undo_init(UndoStack* u);
void undo_push(UndoStack* u, const char* snapshot);
char* undo_pop(UndoStack* u);
void undo_free(UndoStack* u);

#endif // UNDO_H
