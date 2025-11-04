#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <stddef.h>

typedef struct { char** sentences; size_t count; } FileContent;

FileContent parse_sentences(const char* text);
char* join_sentences(FileContent fc);
void free_filecontent(FileContent fc);

#endif // FILE_OPS_H
