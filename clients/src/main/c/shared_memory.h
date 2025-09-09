#define BACKOFF_PROF 
#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

// #define _POSIX_C_SOURCE 200809L
#include "tls_profiler.h" // cpu relax 호출용
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdalign.h>
#include <time.h>     // nanosleep, struct timespec
#include <sched.h>    // sched_yield
#include <inttypes.h>  // PRIu64

#define BUF_COUNT 4096
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
    
    alignas(64) uint32_t offset[BUF_COUNT];
} LockFreeRingBuffer;

typedef struct {
    LockFreeRingBuffer *rb;
    sem_t *semaphore;
} SharedMemoryHandle;


static inline void cpu_relax(void) {
#ifndef BACKOFF_PROF  // 백오프 비활성화 시, 단순 relax 1회만
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
    #elif defined(__aarch64__)
        __asm__ __volatile__("yield");
    #endif
#else
    // increment_relax_count();
    // print_relax_count();
    // 지수 백오프: 최대 1 << 10 = 1024 반복

    uint64_t backoff_iters = 1ULL << (t_bk_relax_count < 12 ? t_bk_relax_count : 12);
    backoff_iters = backoff_iters / 2;
    
    uint64_t jitter = (thread_id * 37) & 0xF; // 스레드 순서 기반 (0~15)
    backoff_iters = backoff_iters + jitter;
    
    for (uint64_t i = 0; i < backoff_iters; ++i) {
        #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
        #elif defined(__aarch64__)
            __asm__ __volatile__("yield");
        #endif
    }
#endif
}

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length);


#endif
