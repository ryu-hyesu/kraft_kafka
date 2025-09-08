#define BACKOFF_PROF  // 이 줄 추가

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

#define SHM_NAME "/shm_pool_region"

_Atomic uint64_t pool_get_cnt = 0, pool_rel_cnt = 0;

shm_pool_region_t *g_pool = NULL;

static inline size_t expected_region_size(void){
    return sizeof(shm_pool_region_t);
}

_Static_assert(POOL_COUNT != 0 && ((POOL_COUNT & (POOL_COUNT - 1)) == 0),
               "POOL_COUNT must be nonzero power of two");

_Static_assert(BUF_COUNT && ((BUF_COUNT & (BUF_COUNT-1)) == 0),
"BUF_COUNT must be power of 2");

// 0 = uninit, 1 = initing, 2 = ready, -1 = failed
static _Atomic int g_pool_inited = 0; // process-local fast path

int init_shared_memory_pool(void) {
    // 로컬 상태 빠른 분기
    int s = atomic_load_explicit(&g_pool_inited, memory_order_relaxed);
    if (s == 2) return 0;
    if (s == -1) return -1;
    if (s == 1) {
        // 다른 스레드가 mmap 중. 유한 대기 권장(여긴 간단히 busy-wait)
        while (atomic_load_explicit(&g_pool_inited, memory_order_relaxed) == 1) cpu_relax();
        return atomic_load_explicit(&g_pool_inited, memory_order_relaxed) == 2 ? 0 : -1;
    }

    // 내가 초기화자에 도전
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_pool_inited, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        // 이미 누가 1 또는 2로 바꿈 → 결과를 기다림
        while (atomic_load_explicit(&g_pool_inited, memory_order_relaxed) == 1) cpu_relax();
        return atomic_load_explicit(&g_pool_inited, memory_order_relaxed) == 2 ? 0 : -1;
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

        for (uint32_t i = 0; i < POOL_COUNT; ++i) {
            meta->slots[i] = i;
            atomic_store_explicit(&meta->pub_seq[i], 0, memory_order_relaxed);
        }

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
    if (atomic_load_explicit(&g_pool_inited, memory_order_relaxed) != 2) {
        if (init_shared_memory_pool() != 0) return NULL;
    }
    // 권위 게이트(공유) 확인
    if (atomic_load_explicit(&g_pool->meta.init_state, memory_order_acquire) != 2) {
        // 유한 대기 권장. 간단히 실패 반환.
        return NULL;
    }

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
        cpu_relax();
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
    fflush(stderr);
    return 0;
}

static inline void try_advance_head_pub(volatile shm_pool_meta_t *m)
{
    const uint32_t mask = POOL_COUNT - 1;

    for (;;) {
        // 1) 현재 프런티어를 읽음 (relaxed로 충분)
        uint64_t cur  = atomic_load_explicit(&m->head_pub, memory_order_relaxed);
        uint64_t next = cur;

        // 2) 연속 구간 탐색
        //    pub_seq[next & mask] 가 (next+1) 이면 해당 티켓이 준비됨 → 한 칸 전진
        for (;;) {
            uint64_t tag = atomic_load_explicit(&m->pub_seq[next & mask],
                                                memory_order_acquire);
            if (tag != next + 1) break;   // 연속이 끊기면 중단
            ++next;
        }

        if (next == cur) {
            // 더 당길 게 없으면 종료
            return;
        }

        // 3) head_pub을 next로 한번에 전진 (성공 시 release)
        //    실패하면 다른 스레드가 이미 갱신한 것 → 처음부터 재시도
        if (atomic_compare_exchange_weak_explicit(&m->head_pub, &cur, next,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
        // CAS 실패: cur 값이 갱신되었으니 루프 재시작
    }
}

alignas(64) static _Atomic int advance_lock = 0;
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

    // 1) 예약 (원자성 보장해서 가져와 CAS연산이 필요 없음)
    uint64_t my = atomic_fetch_add_explicit(&g_pool->meta.head_resv, 1, memory_order_relaxed);
    uint32_t pos = (uint32_t)(my & (POOL_COUNT - 1)); // 인덱스 계산

    // 2) 용량 확인
    while ((int64_t)(my - atomic_load_explicit(&g_pool->meta.head_pub, memory_order_acquire))
            >= (int64_t)POOL_COUNT) {
        cpu_relax();
    }

    // 3) 슬롯 작성
    g_pool->meta.slots[pos] = idx;

    // 4) 슬롯 게시 표시
    atomic_store_explicit(&g_pool->meta.pub_seq[pos], my + 1, memory_order_release);
    
    // 3) head_pub 가져옴 (여기서 head_pub에 접근하기 위해 경합 발생함)
    // int spin = 0;
    // while (atomic_load_explicit(&g_pool->meta.head_pub, memory_order_relaxed) != my) {
    //     backoff_spin(++spin);
    // }
    
    // 한 놈만 민다!!
    uint64_t hp = atomic_load_explicit(&g_pool->meta.head_pub, memory_order_relaxed);
    if (my == hp) {
        // 대표 한 놈만 '연속 구간'을 스캔해서 head_pub를 당긴다
        if (atomic_exchange_explicit(&advance_lock, 1, memory_order_acq_rel) == 0) {
            try_advance_head_pub(&g_pool->meta);   // 연속으로 준비된 구간 끝까지 한 번에 밀기
            atomic_store_explicit(&advance_lock, 0, memory_order_release);
        }
    }
    // try_advance_head_pub(); 
}


