#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>

#define BUF_COUNT 128
#define BUF_SIZE 4096

typedef struct {
    _Atomic uint64_t prod_seq;
    _Atomic uint64_t cons_seq;
    uint32_t offset[BUF_COUNT];   // ✅ 메시지를 가리키는 포인터
} LockFreeRingBuffer;

typedef struct {
    LockFreeRingBuffer *rb;
    sem_t *semaphore;
} SharedMemoryHandle;

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length);


#endif
