#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>

#define BUF_COUNT 100
#define BUF_SIZE 1024

typedef struct {
    _Atomic uint64_t prod_seq;
    _Atomic uint64_t cons_seq;
    char data[BUF_COUNT][BUF_SIZE];
} LockFreeRingBuffer;

extern LockFreeRingBuffer *rb;
extern sem_t *semaphore;

int initialize_shared_memory(const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, char *out, int *out_length);

#endif
