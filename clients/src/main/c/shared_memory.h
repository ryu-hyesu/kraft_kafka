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

/*
// ===== Backoff metrics (옵션) =====
#ifdef BACKOFF_PROF

// ==== 최소 타이머 (총 릴리즈 시간 + 용량대기 시간 합계만) ====
extern _Atomic uint64_t g_rel_total_ns, g_rel_cnt, g_wait_total_ns;

extern _Atomic uint64_t g_enq_total_ns;
extern _Atomic uint64_t g_enq_cnt;
extern _Atomic uint64_t g_enq_cap_wait_ns;     // 용량 대기(consumer가 못 따라와서 full일 때)
extern _Atomic uint64_t g_enq_pub_cas_wait_ns; // prod_pub CAS 전후 대기

static inline uint64_t bk_now_ns(void){
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
    }
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void bk_enq_add(uint64_t total_ns,
                              uint64_t cap_wait_ns,
                              uint64_t pubcas_wait_ns){
    atomic_fetch_add(&g_enq_total_ns, total_ns);
    atomic_fetch_add(&g_enq_cnt, 1);
    atomic_fetch_add(&g_enq_cap_wait_ns, cap_wait_ns);
    atomic_fetch_add(&g_enq_pub_cas_wait_ns, pubcas_wait_ns);
}

// --- ALLOCATE ---
extern _Atomic uint64_t g_alloc_total_ns;
extern _Atomic uint64_t g_alloc_cnt;
extern _Atomic uint64_t g_alloc_cap_wait_ns;     // 용량 대기(consumer가 못 따라와서 full일 때)
extern _Atomic uint64_t g_alloc_pub_cas_wait_ns; // prod_pub CAS 전후 대기

static inline void bk_alloc_add(uint64_t total_ns,
                              uint64_t cap_wait_ns,
                              uint64_t pubcas_wait_ns){
    atomic_fetch_add(&g_alloc_total_ns, total_ns);
    atomic_fetch_add(&g_alloc_cnt, 1);
    atomic_fetch_add(&g_alloc_cap_wait_ns, cap_wait_ns);
    atomic_fetch_add(&g_alloc_pub_cas_wait_ns, pubcas_wait_ns);
}

// --- DEQ ---
extern _Atomic uint64_t g_deq_total_ns;
extern _Atomic uint64_t g_deq_cnt;
extern _Atomic uint64_t g_deq_cas_wait_ns;     // cons_seq CAS 실패로 인한 대기

static inline void bk_deq_add(uint64_t total_ns,
                              uint64_t cas_wait_ns){
    atomic_fetch_add(&g_deq_total_ns, total_ns);
    atomic_fetch_add(&g_deq_cnt, 1);
    atomic_fetch_add(&g_deq_cas_wait_ns, cas_wait_ns);
}


/* 확장 출력 
static inline void bk_print_times_ext(void){
    // allocate
    uint64_t alloccnt = atomic_load(&g_alloc_cnt);
    uint64_t allocns  = atomic_load(&g_alloc_total_ns);
    uint64_t alloccap = atomic_load(&g_alloc_cap_wait_ns);
    uint64_t allocpub = atomic_load(&g_alloc_pub_cas_wait_ns);
    double alloc_avg_ns     = alloccnt ? (double)allocns/alloccnt   : 0.0;
    double alloc_cap_avg_ns = alloccnt ? (double)alloccap/alloccnt  : 0.0;
    double alloc_pub_avg_ns = alloccnt ? (double)allocpub/alloccnt  : 0.0;

    // enqueue
    uint64_t enqcnt = atomic_load(&g_enq_cnt);
    uint64_t enqns  = atomic_load(&g_enq_total_ns);
    uint64_t enqcap = atomic_load(&g_enq_cap_wait_ns);
    uint64_t enqpub = atomic_load(&g_enq_pub_cas_wait_ns);
    double enq_avg_ns     = enqcnt ? (double)enqns/enqcnt   : 0.0;
    double enq_cap_avg_ns = enqcnt ? (double)enqcap/enqcnt  : 0.0;
    double enq_pub_avg_ns = enqcnt ? (double)enqpub/enqcnt  : 0.0;

    // dequeue
    uint64_t deqcnt = atomic_load(&g_deq_cnt);
    uint64_t deqns  = atomic_load(&g_deq_total_ns);
    uint64_t deqcas = atomic_load(&g_deq_cas_wait_ns);
    double deq_avg_ns     = deqcnt ? (double)deqns/deqcnt   : 0.0;
    double deq_cas_avg_ns = deqcnt ? (double)deqcas/deqcnt  : 0.0;

    fprintf(stderr,
      "[time] alloc_avg=%.1f ns (calls=%" PRIu64 ") | alloc_total=%.3f ms | "
      "alloc_cap_wait_avg=%.1f ns | alloc_pub_cas_wait_avg=%.1f ns\n",
      alloc_avg_ns, alloccnt, (double)allocns/1e6, alloc_cap_avg_ns, alloc_pub_avg_ns);

    fprintf(stderr,
      "[time] enq_avg=%.1f ns (calls=%" PRIu64 ") | enq_total=%.3f ms | "
      "enq_cap_wait_avg=%.1f ns | enq_pub_cas_wait_avg=%.1f ns\n",
      enq_avg_ns, enqcnt, (double)enqns/1e6, enq_cap_avg_ns, enq_pub_avg_ns);

    fprintf(stderr,
      "[time] deq_avg=%.1f ns (calls=%" PRIu64 ") | deq_total=%.3f ms | "
      "deq_cas_wait_avg=%.1f ns\n",
      deq_avg_ns, deqcnt, (double)deqns/1e6, deq_cas_avg_ns);
}

static inline void bk_print_times(void){
    uint64_t cnt   = atomic_load(&g_rel_cnt);
    uint64_t relns = atomic_load(&g_rel_total_ns);
    uint64_t waitns= atomic_load(&g_wait_total_ns);

    double rel_avg_ns = cnt ? (double)relns / (double)cnt : 0.0;

    fprintf(stderr,
      "[time] release_avg=%.1f ns (calls=%" PRIu64 ") | "
      "release_total=%.3f ms | wait_total=%.3f ms\n",
      rel_avg_ns, cnt, (double)relns/1e6, (double)waitns/1e6);
}

extern _Atomic uint64_t g_bk_relax_tot, g_bk_yield_tot, g_bk_nanos_tot;
extern _Atomic uint64_t g_bk_yield_ns_tot, g_bk_nanos_ns_tot;

static inline void backoff_print_totals(void){
    uint64_t relax = atomic_load(&g_bk_relax_tot);
    uint64_t yield = atomic_load(&g_bk_yield_tot);
    uint64_t nanos = atomic_load(&g_bk_nanos_tot);
    uint64_t y_ns  = atomic_load(&g_bk_yield_ns_tot);
    uint64_t n_ns  = atomic_load(&g_bk_nanos_ns_tot);
    double y_avg_us = yield ? (double)y_ns / yield / 1000.0 : 0.0;
    double n_avg_us = nanos ? (double)n_ns / nanos / 1000.0 : 0.0;

    fprintf(stderr,
      "[backoff] relax=%" PRIu64 " | yield=%" PRIu64 " (avg %.2f us) | "
      "nanos=%" PRIu64 " (avg %.2f us)\n",
      relax, yield, y_avg_us, nanos, n_avg_us);
}
#endif
*/

/* 스레드 로컬 카운터
#ifdef BACKOFF_PROF

// 전역 카운터: 스레드 ID 할당용
static atomic_uint g_thread_id_counter = 0;

// 스레드 로컬: 각 스레드의 고유 ID 및 relax 카운트
static __thread uint32_t thread_id;
static __thread uint64_t t_bk_relax_count = 0;

// 스레드별 ID 초기화 함수
static inline void init_thread_id(void) {
    static __thread int initialized = 0;
    if (!initialized) {
        thread_id = atomic_fetch_add(&g_thread_id_counter, 1);
        initialized = 1;
    }
}

#endif

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    
#endif

#ifdef BACKOFF_PROF
    increment_relax_count();
    print_relax_count();
#endif
}
*/

static inline void cpu_relax(void) {
#ifndef BACKOFF_PROF  // 백오프 비활성화 시, 단순 relax 1회만
    #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
    #elif defined(__aarch64__)
        __asm__ __volatile__("yield");
    #endif
#else
    increment_relax_count();
    print_relax_count();
    // 지수 백오프: 최대 1 << 10 = 1024 반복

    uint64_t backoff_iters = 1ULL << (t_bk_relax_count < 12 ? t_bk_relax_count : 12);
    backoff_iters = backoff_iters / 2;
    
    uint64_t jitter = (thread_id * 37) & 0xF; // 스레드 순서 기반 (0~15)
    backoff_iters = backoff_iters + jitter;
    
    fprintf(stderr, "[BackOff Iters] %" PRIu64 " [jitter]\n", backoff_iters);
    for (uint64_t i = 0; i < backoff_iters; ++i) {
        #if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
        #elif defined(__aarch64__)
            __asm__ __volatile__("yield");
        #endif
    }
#endif
}

/*
#ifdef BACKOFF_PROF
static inline void print_cpu_relax_stats(void) {
    fprintf(stderr, "[Thread %u] cpu_relax() calls: %" PRIu64 "\n",
            thread_id, t_bk_relax_count);
}
#endif
*/



/*
static inline void backoff_spin(int spin) {
#ifdef BACKOFF_PROF
    if (spin < 512) {
        cpu_relax();
        atomic_fetch_add(&g_bk_relax_tot, 1);
        return;
    }
    if (spin < 4096) {
        uint64_t t0 = bk_now_ns();
        sched_yield();
        atomic_fetch_add(&g_bk_yield_tot, 1);
        atomic_fetch_add(&g_bk_yield_ns_tot, bk_now_ns() - t0);
        return;
    }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 };
    uint64_t t0 = bk_now_ns();
    nanosleep(&ts, NULL);
    atomic_fetch_add(&g_bk_nanos_tot, 1);
    atomic_fetch_add(&g_bk_nanos_ns_tot, bk_now_ns() - t0);
#else
    if (spin < 512) { cpu_relax(); return; }
    if (spin < 4096) { sched_yield(); return; }
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000 };
    nanosleep(&ts, NULL);
#endif
}
*/

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create);
void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name);
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length);
bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length);


#endif
