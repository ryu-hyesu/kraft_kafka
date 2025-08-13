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
#include <stdatomic.h>
#include <stdbool.h>

// 0 = uninit, 1 = initing, 2 = ready, -1 = failed
// static _Atomic int g_pool_inited = 0; // local variable

#define SHM_NAME "/shm_pool_region"
// #define INIT_MAGIC 0xC0FFEE01u

// unsigned char (*shm_pool)[SAMPLE_SIZE];  // 포인터 to array-of-SAMPLE_SIZE
// static shm_memory_pool freelist;
_Atomic uint64_t pool_get_cnt = 0, pool_rel_cnt = 0;

shm_pool_region_t *g_pool = NULL;

static inline size_t expected_region_size(void){
    return sizeof(shm_pool_region_t);
}

_Static_assert(POOL_COUNT != 0 && ((POOL_COUNT & (POOL_COUNT - 1)) == 0),
               "POOL_COUNT must be nonzero power of two");
static inline uint32_t ring_next(uint32_t x) {
    // 안전하게 맞추려면 % 사용; 성능 튜닝은 나중에 2^n로 바꿔 마스크
    return (x + 1) % (2u * POOL_COUNT);
}

// 0 = uninit, 1 = initing, 2 = ready, -1 = failed
static _Atomic int g_pool_inited = 0; // process-local fast path

int init_shared_memory_pool(void) {
    // 로컬 상태 빠른 분기
    int s = atomic_load_explicit(&g_pool_inited, memory_order_acquire);
    if (s == 2) return 0;
    if (s == -1) return -1;
    if (s == 1) {
        // 다른 스레드가 mmap 중. 유한 대기 권장(여긴 간단히 busy-wait)
        while (atomic_load_explicit(&g_pool_inited, memory_order_acquire) == 1) cpu_relax();
        return atomic_load_explicit(&g_pool_inited, memory_order_acquire) == 2 ? 0 : -1;
    }

    // 내가 초기화자에 도전
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_pool_inited, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        // 이미 누가 1 또는 2로 바꿈 → 결과를 기다림
        while (atomic_load_explicit(&g_pool_inited, memory_order_acquire) == 1) cpu_relax();
        return atomic_load_explicit(&g_pool_inited, memory_order_acquire) == 2 ? 0 : -1;
    }

    // ---- 여기서부터 실제 shm open/map/검증/초기화 ----
    const char *name = SHM_NAME;
    size_t sz = expected_region_size();

    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { goto fail; }

    if (ftruncate(fd, (off_t)sz) == -1) {
        close(fd);
        goto fail;
    }

    void *addr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved_errno = errno;
    close(fd);
    if (addr == MAP_FAILED || addr == NULL) {
        fprintf(stderr, "[SHM][ERROR] mmap(size=%zu) failed: %s (addr=%p)\n",
                sz, strerror(saved_errno), addr);
        goto fail;
    }

    g_pool = (shm_pool_region_t *)addr;
    shm_pool_meta_t *meta = &g_pool->meta;

    uint32_t state = atomic_load_explicit(&meta->init_state, memory_order_acquire);
    bool need_init =
        (state == 0) ||
        meta->magic       != SHM_MAGIC   ||
        meta->ver         != SHM_VER     ||
        meta->pool_count  != POOL_COUNT  ||
        meta->sample_size != SAMPLE_SIZE ||
        meta->region_size != sz;

    if (need_init) {
        uint32_t expect = state;
        if (!atomic_compare_exchange_strong_explicit(
                &meta->init_state, &expect, 1,
                memory_order_acq_rel, memory_order_relaxed)) {
            // 다른 프로세스/스레드가 초기화 중 → 완료 대기
            while (atomic_load_explicit(&meta->init_state, memory_order_acquire) != 2) cpu_relax();
            atomic_store_explicit(&g_pool_inited, 2, memory_order_release);
            return 0;
        }

        // 실제 초기화: 데이터 영역만 0
        memset(&g_pool->data[0][0], 0, sizeof(g_pool->data));
        meta->magic       = SHM_MAGIC;
        meta->ver         = SHM_VER;
        meta->flags       = 0;
        meta->pool_count  = POOL_COUNT;
        meta->sample_size = SAMPLE_SIZE;
        meta->region_size = sz;

        for (uint32_t i = 0; i < POOL_COUNT; ++i) meta->slots[i] = i;

        atomic_store_explicit(&meta->tail, 0,                       memory_order_relaxed);
        atomic_store_explicit(&meta->head_resv, (uint64_t)POOL_COUNT,    memory_order_relaxed);
        atomic_store_explicit(&meta->head_pub, (uint64_t)POOL_COUNT,    memory_order_relaxed);
        atomic_fetch_add_explicit(&meta->init_epoch, 1,             memory_order_relaxed);

        // 공유 게이트 공개 (반드시 마지막, release)
        atomic_store_explicit(&meta->init_state, 2, memory_order_release);

        // 로컬 fast-path 공개
        atomic_store_explicit(&g_pool_inited, 2, memory_order_release);
        return 0;
    }

    // need_init == false
    if (state == 1) {
        while (atomic_load_explicit(&meta->init_state, memory_order_acquire) != 2) cpu_relax();
    }

    // 여기 도달 = 공유 측 READY 보장 → 로컬 fast-path READY
    atomic_store_explicit(&g_pool_inited, 2, memory_order_release);
    return 0;

fail:
    atomic_store_explicit(&g_pool_inited, -1, memory_order_release);
    return -1;
}


// 메모리 가져옴 (freelist pop)
unsigned char* shm_pool_get() {
    // 로컬 fast path 체크
    if (atomic_load_explicit(&g_pool_inited, memory_order_acquire) != 2) {
        if (init_shared_memory_pool() != 0) return NULL;
    }
    // 권위 게이트(공유) 확인
    if (atomic_load_explicit(&g_pool->meta.init_state, memory_order_acquire) != 2) {
        // 유한 대기 권장. 간단히 실패 반환.
        return NULL;
    }

    int attempt = 0;
    for (;;) {
        uint64_t tail = atomic_load_explicit(&g_pool->meta.tail, memory_order_relaxed);
        uint64_t pub = atomic_load_explicit(&g_pool->meta.head_pub, memory_order_acquire);

        if (pub == tail) {
            // 빈 상태
            return NULL;
        }

        uint64_t new_tail = tail + 1;
        if (atomic_compare_exchange_strong_explicit(
                &g_pool->meta.tail, &tail, new_tail,
                memory_order_acquire,  // 성공 시 이후 읽기 가시성 확보
                memory_order_relaxed)) {
            uint32_t idx = g_pool->meta.slots[(uint32_t)tail & (POOL_COUNT - 1)];
            if (__builtin_expect(idx >= POOL_COUNT, 0)) {
                fprintf(stderr, "❌FATAL: idx OOB idx=%u tail=%" PRIu64 " head=%" PRIu64 "\n", idx, tail, pub);
                abort();
            }
            unsigned char* p = &g_pool->data[idx][0];
            return p;
        }
        backoff_spin(++attempt);
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



// 메모리 반납 (freelist push)
// ⚠️ 현재 구현은 다중 프로듀서에서 slot 쓰기 경쟁이 생길 수 있음.
// 근본 해결은 head_resv/pub 이중 포인터 또는 per-slot seq(권장).
void shm_pool_release(unsigned char* ptr) {
    if (!g_pool || !ptr) return;

    // ptr -> idx 역산
    uintptr_t base = (uintptr_t)&g_pool->data[0][0];
    uintptr_t end  = (uintptr_t)&g_pool->data[POOL_COUNT][0];
    uintptr_t up   = (uintptr_t)ptr;
    if (up < base || up >= end) { fprintf(stderr,"[REL][FAIL OOR]\n"); return; }
    size_t off = (size_t)(up - base);
    if (off % SAMPLE_SIZE)       { fprintf(stderr,"[REL][FAIL MISALN]\n"); return; }
    
    uint32_t idx = (uint32_t)(off / SAMPLE_SIZE);

    uint64_t my = atomic_fetch_add_explicit(&g_pool->meta.head_resv, 1, memory_order_acq_rel);

    g_pool->meta.slots[(uint32_t)my & (POOL_COUNT - 1)] = idx;

    while (atomic_load_explicit(&g_pool->meta.head_pub, memory_order_acquire) != my) {
        cpu_relax();
    }
    
    atomic_store_explicit(&g_pool->meta.head_pub, my + 1, memory_order_release);

    // for (;;) {
    //     uint64_t head = 
    //     // 1) 먼저 슬롯에 데이터 기록
    //     g_pool->meta.slots[(uint32_t)head & (POOL_COUNT - 1)] = idx;

    //     // 2) 공개(head++) — release로 publish
    //     uint64_t new_head = head + 1;
    //     if (atomic_compare_exchange_strong_explicit(
    //             &g_pool->meta.head, &head, new_head,
    //             memory_order_release, memory_order_relaxed)) {
    //         return;
    //     }

    //     // ❗ CAS 경합 시 동일 슬롯에 다중 쓰기 위험 존재.
    //     // 실전에선 head_resv(atomic_fetch_add)로 예약 후, per-slot seq로 공개 권장.
    //     cpu_relax();
    // }
}


