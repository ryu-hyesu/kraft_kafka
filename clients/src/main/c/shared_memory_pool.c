#include "shared_memory_pool.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>   // pthread_self

#define SHM_NAME "/shm_pool_region"
#define INIT_MAGIC 0xC0FFEE01u

// unsigned char (*shm_pool)[SAMPLE_SIZE];  // 포인터 to array-of-SAMPLE_SIZE
// static shm_memory_pool freelist;
_Atomic uint64_t pool_get_cnt = 0, pool_rel_cnt = 0;


shm_pool_region_t *g_pool = NULL;

static inline uint32_t ring_next(uint32_t x) {
    // 안전하게 맞추려면 % 사용; 성능 튜닝은 나중에 2^n로 바꿔 마스크
    return (x + 1) % (2u * POOL_COUNT);
}



int init_shared_memory_pool(void) {
    const char *name = "/shm_pool_region";
    size_t sz = sizeof(shm_pool_region_t);

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        fprintf(stderr, "[SHM][ERROR] shm_open(%s): %s\n", name, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)sz) == -1) {
        fprintf(stderr, "[SHM][ERROR] ftruncate(%zu): %s\n", sz, strerror(errno));
        close(fd);
        return -1;
    }

    void *addr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved_errno = errno; // mmap 이후 errno 보존
    close(fd);

    if (addr == MAP_FAILED || addr == NULL) {
        fprintf(stderr, "[SHM][ERROR] mmap(size=%zu) failed: %s (addr=%p)\n",
                sz, strerror(saved_errno), addr);
        return -1;
    }

    g_pool = (shm_pool_region_t *)addr;
    fprintf(stderr, "[SHM] mmap OK: g_pool=%p size=%zu\n", (void*)g_pool, sz);

    // 최초 초기화: init_magic으로 가드
    uint32_t expect = 0xCAFEBABE;
    if (atomic_load_explicit(&g_pool->meta.init_magic, memory_order_acquire) != expect) {
        memset(g_pool, 0, sz);
        for (uint32_t i = 0; i < POOL_COUNT; ++i) g_pool->meta.slots[i] = i;
        atomic_store_explicit(&g_pool->meta.tail, 0,          memory_order_relaxed);
        atomic_store_explicit(&g_pool->meta.head, POOL_COUNT, memory_order_relaxed);
        atomic_store_explicit(&g_pool->meta.init_magic, expect, memory_order_release);
        fprintf(stderr, "[SHM] init pool meta once (POOL_COUNT=%d, SAMPLE_SIZE=%d)\n",
                POOL_COUNT, SAMPLE_SIZE);
    }

    return 0;
}

// 메모리 가져옴 (read)
unsigned char* shm_pool_get() {
    if(!g_pool) return NULL;
    int attempt = 0;

    while (1) {
        uint32_t tail = atomic_load_explicit(&g_pool->meta.tail, memory_order_acquire);
        uint32_t head = atomic_load_explicit(&g_pool->meta.head, memory_order_acquire);

        // 빈 상태 체크는 이게 더 명확함
        if ((uint32_t)(head - tail) == 0) {
            fprintf(stderr, "❌ [POOL] shm_pool exhausted: tail=%u, head=%u\n", tail, head);
            return NULL;
        }

        uint32_t slot_pos = tail % POOL_COUNT;
        uint32_t new_tail = tail + 1;

        if (atomic_compare_exchange_weak_explicit(
                &g_pool->meta.tail, &tail, new_tail,
                memory_order_acquire,  // 여기서 acquire: 이후 읽기 가시성 확보
                memory_order_relaxed)) {

            // CAS 성공 후에 슬롯에서 인덱스 읽기
            atomic_thread_fence(memory_order_acquire);
            uint32_t idx = g_pool->meta.slots[slot_pos];

            atomic_fetch_add_explicit(&pool_get_cnt, 1, memory_order_relaxed);

            // fprintf(stderr,
            //     "[POOL][GET][pid=%d] g_pool=%p meta=%p init_magic=%08x head=%u tail=%u POOL_COUNT=%d\n",
            //     getpid(), g_pool, &g_pool->meta,
            //     atomic_load(&g_pool->meta.init_magic),
            //     atomic_load(&g_pool->meta.head),
            //     atomic_load(&g_pool->meta.tail),
            //     POOL_COUNT);

            return &g_pool->data[idx][0];
        }

        if (++attempt > 100000) {
            fprintf(stderr, "❌ [POOL] shm_pool_get CAS failed too many times\n");
            return NULL;
        }
    }
}

static _Atomic uint64_t g_rel_ok = 0, g_rel_fail = 0;

static inline int ptr_to_index(const unsigned char *p, uint32_t *out_idx) {
    uintptr_t base = (uintptr_t)&g_pool->data[0][0];
    uintptr_t end  = (uintptr_t)&g_pool->data[POOL_COUNT][0];
    uintptr_t up   = (uintptr_t)p;

    if (up < base || up >= end) {
        atomic_fetch_add_explicit(&g_rel_fail, 1, memory_order_relaxed);
        fprintf(stderr,"[REL][FAIL OOR] ptr=%p base=%p end=%p\n",(void*)up,(void*)base,(void*)end);
        fflush(stderr);
        return -1;
    }
    size_t off   = (size_t)(up - base);
    size_t delta = off % SAMPLE_SIZE;
    if (delta) {
        size_t idx_guess = off / SAMPLE_SIZE;
        atomic_fetch_add_explicit(&g_rel_fail, 1, memory_order_relaxed);
        fprintf(stderr,"[REL][FAIL MISALN] ptr=%p off=%zu delta=%zu idx_guess=%zu\n",
                (void*)up, off, delta, idx_guess);
        fflush(stderr);
        return -1;
    }
    *out_idx = (uint32_t)(off / SAMPLE_SIZE);
    atomic_fetch_add_explicit(&g_rel_ok, 1, memory_order_relaxed);
    // fprintf(stderr,"[REL][OK] idx=%u ptr=%p\n", *out_idx, (void*)up);
    fflush(stderr);
    return 0;
}



// 메모리 돌려줌 (write)
void shm_pool_release(unsigned char* ptr) {
    if (!g_pool || !ptr) return;

    uint32_t idx;
    if (ptr_to_index(ptr, &idx) != 0) {
        fprintf(stderr, "ERROR: Invalid pointer in release\n");
        return;
    }

    while (1) {
        uint32_t tail = atomic_load_explicit(&g_pool->meta.tail, memory_order_acquire); // FOR log
        uint32_t head = atomic_load_explicit(&g_pool->meta.head, memory_order_relaxed);
        uint32_t slot_pos = head % POOL_COUNT;

        // 1) 데이터(인덱스) 먼저 기록
        g_pool->meta.slots[slot_pos] = idx;

        // 2) 퍼블리시 배리어: 이후의 관찰자(get)가 이 쓰기를 볼 수 있게
        atomic_thread_fence(memory_order_release);


        // 3) 인덱스 증가(공개)
        uint32_t new_head = head + 1;
        if (atomic_compare_exchange_weak_explicit(
                &g_pool->meta.head, &head, new_head,
                memory_order_release, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&pool_rel_cnt, 1, memory_order_relaxed);

            // fprintf(stderr,
            //     "[POOL][REL!][pid=%d] g_pool=%p meta=%p init_magic=%08x head=%u tail=%u POOL_COUNT=%d\n",
            //     getpid(), g_pool, &g_pool->meta,
            //     atomic_load(&g_pool->meta.init_magic),
            //     atomic_load(&g_pool->meta.head),
            //     atomic_load(&g_pool->meta.tail),
            //     POOL_COUNT);
            return;
        }
        // CAS 실패 시 재시도: 새 head로 다시 slot_pos/슬롯 기록을 해야 하므로 루프 상단으로
    }
}

