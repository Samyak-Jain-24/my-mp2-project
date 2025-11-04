#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "file_ops.h"

static void push_sentence(FileContent* fc, const char* start, size_t len) {
    if (len == 0) return;
    fc->sentences = (char**)realloc(fc->sentences, (fc->count + 1) * sizeof(char*));
    char* s = (char*)malloc(len + 1);
    memcpy(s, start, len); s[len] = '\0';
    fc->sentences[fc->count++] = s;
}

FileContent parse_sentences(const char* text) {
    FileContent fc; fc.sentences = NULL; fc.count = 0;
    if (!text) return fc;
    const char* p = text; const char* sen_start = p;
    while (*p) {
        if (*p == '.' || *p == '!' || *p == '?') {
            // include punctuation
            p++;
            // also include trailing spaces after punctuation to preserve spacing
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            push_sentence(&fc, sen_start, (size_t)(p - sen_start));
            sen_start = p;
        } else {
            p++;
        }
    }
    // leftover
    if (p > sen_start) push_sentence(&fc, sen_start, (size_t)(p - sen_start));
    return fc;
}

char* join_sentences(FileContent fc) {
    size_t total = 0;
    for (size_t i = 0; i < fc.count; i++) total += strlen(fc.sentences[i]);
    char* out = (char*)malloc(total + 1);
    char* q = out;
    for (size_t i = 0; i < fc.count; i++) {
        size_t len = strlen(fc.sentences[i]);
        memcpy(q, fc.sentences[i], len); q += len;
    }
    *q = '\0';
    return out;
}

void free_filecontent(FileContent fc) {
    for (size_t i=0;i<fc.count;i++) free(fc.sentences[i]);
    free(fc.sentences);
}
