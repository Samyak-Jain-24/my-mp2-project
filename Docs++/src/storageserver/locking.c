#include "locking.h"

void locking_init(SentenceLock* l) { pthread_mutex_init(&l->mutex, NULL); }
void locking_lock(SentenceLock* l) { pthread_mutex_lock(&l->mutex); }
void locking_unlock(SentenceLock* l) { pthread_mutex_unlock(&l->mutex); }
