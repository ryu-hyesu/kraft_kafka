#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdalign.h>
#include <time.h>     // nanosleep, struct timespec
#include <sched.h>    // sched_yield
#define BUF_COUNT 4096
#define BUF_SIZE 32768
#define _POSIX_C_SOURCE 200809L

extern _Atomic uint64_t enq_success_count;
extern _Atomic uint64_t deq_success_count;

typedef struct {
    alignas(64) _Atomic uint32_t prod_resv; //  
    char _pad0[64 - sizeof(_Atomic uint32_t)];

    alignas(64) _Atomic uint32_t prod_pub;
    char _pad1[64 - sizeof(_Atomic uint32_t)];

    alignas(64) _Atomic uint32_t cons_seq;
    char _pad2[64 - sizeof(_Atomic uint32_t)];
    
    alignas(64) uint32_t offset[BUF_COUNT];
} LockFreeRingBuffer;

typedef struct {
    LockFreeRingBuffer *rb;
    sem_t *semaphore;
} SharedMemoryHandle;

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    ; // no-op
#endif
}


static inline void backoff_spin(int spin) {
    // fprintf(stderr,"[back off] %d\n", spin);
    if (spin < 512) { cpu_relax(); return; }
    if (spin < 4096) { sched_yield(); return; }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 }; // 0.2ms
    nanosleep(&ts, NULL);
}


int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length);


#endif
