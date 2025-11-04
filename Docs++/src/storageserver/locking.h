#ifndef LOCKING_H
#define LOCKING_H

#include <pthread.h>

typedef struct { pthread_mutex_t mutex; } SentenceLock;

void locking_init(SentenceLock* l);
void locking_lock(SentenceLock* l);
void locking_unlock(SentenceLock* l);

#endif // LOCKING_H
