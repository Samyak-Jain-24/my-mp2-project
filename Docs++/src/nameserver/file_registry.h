#ifndef FILE_REGISTRY_H
#define FILE_REGISTRY_H

#include <stddef.h>

typedef struct { const char* name; const char* ss_id; } FileEntry;

void registry_init();
void registry_add(const char* name, const char* ss_id);
const char* registry_lookup(const char* name);
void registry_remove(const char* name);
size_t registry_count();
// iteration helpers
size_t registry_used();
const FileEntry* registry_entry(size_t idx);

#endif // FILE_REGISTRY_H
