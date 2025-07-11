#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>
#include <stdatomic.h>

#define BUF_COUNT 512
#define BUF_SIZE 10000

#define BUF_EMPTY 0
#define BUF_WAITING 1
#define BUF_READY 2
#define BUF_READING 3

typedef struct {
    _Atomic uint64_t seq;
    char data[BUF_SIZE];
} __attribute__((aligned(64))) Buf;

typedef struct {
    _Atomic uint64_t prod_seq;
    _Atomic uint64_t cons_seq;
    Buf buf[BUF_COUNT];
} __attribute__((aligned(64))) LockFreeRingBuffer;

typedef struct {
    LockFreeRingBuffer *rb;
    sem_t *semaphore;
} SharedMemoryHandle;

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, char *out, int *out_length);

#endif

