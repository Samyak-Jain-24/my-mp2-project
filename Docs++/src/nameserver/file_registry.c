#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_registry.h"

#define MAX_FILES 1024
static FileEntry entries[MAX_FILES];
static size_t used = 0;

void registry_init() { used = 0; }
void registry_add(const char* name, const char* ss_id) {
    if (used >= MAX_FILES) return;
    entries[used].name = strdup(name);
    entries[used].ss_id = strdup(ss_id);
    used++;
}
const char* registry_lookup(const char* name) {
    for (size_t i=0;i<used;i++) if (strcmp(entries[i].name, name)==0) return entries[i].ss_id;
    return NULL;
}
void registry_remove(const char* name) {
    for (size_t i=0;i<used;i++) if (strcmp(entries[i].name, name)==0) { entries[i]=entries[used-1]; used--; return; }
}
size_t registry_count() { return used; }
size_t registry_used() { return used; }
const FileEntry* registry_entry(size_t idx) { return idx<used? &entries[idx] : NULL; }
