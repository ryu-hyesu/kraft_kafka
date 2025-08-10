#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdalign.h>

#define BUF_COUNT 1024
#define BUF_SIZE 32768

extern _Atomic uint64_t enq_success_count;
extern _Atomic uint64_t deq_success_count;

typedef struct {
    alignas(64) _Atomic uint32_t prod_resv; //  
    char _pad0[64 - sizeof(_Atomic uint32_t)];

    alignas(64) _Atomic uint32_t prod_pub;
    char _pad1[64 - sizeof(_Atomic uint32_t)];

    alignas(64) _Atomic uint32_t cons_seq;
    char _pad2[64 - sizeof(_Atomic uint32_t)];
    
    alignas(64) uint32_t offset[BUF_COUNT];   // ✅ 메시지를 가리키는 포인터
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
