#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>
#include <stdatomic.h>

#define BUF_COUNT 512
#define BUF_SIZE 4096

typedef struct {
    _Atomic uint64_t prod_seq;
    _Atomic uint64_t cons_seq;
    char data[BUF_COUNT][BUF_SIZE];
} LockFreeRingBuffer;

typedef struct {
    LockFreeRingBuffer *rb;
    sem_t *semaphore;
} SharedMemoryHandle;

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, char *out, int *out_length);

#endif

