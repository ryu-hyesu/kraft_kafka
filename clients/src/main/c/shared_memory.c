
#include "shared_memory.h"
#include "shared_memory_pool.h"
#include <fcntl.h>        // O_CREAT, O_RDWR
#include <sys/mman.h>     // mmap, PROT_READ, MAP_SHARED, MAP_FAILED
#include <sys/stat.h>     // mode_t
#include <unistd.h>       // ftruncate, close
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>   // PRIu64 매크로 사용을 위해 필요
#include <stdatomic.h>

// static _Atomic int g_pool_inited = 0;
_Atomic uint64_t enq_success_count = 0;
_Atomic uint64_t deq_success_count = 0;


int initialize_memory_pool() {
    if (init_shared_memory_pool() != 0) {
        fprintf(stderr, "❌ Failed to initialize shm_pool\n");
        return -1;
    }

    return 0;
}

int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create) {
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int shm_fd = shm_open(shm_name, flags, S_IRUSR | S_IWUSR);
    if (shm_fd == -1) {
        perror("shm_open");
        return -1;
    }

    if (create && ftruncate(shm_fd, sizeof(LockFreeRingBuffer)) == -1) {
        perror("ftruncate");
        close(shm_fd);
        return -1;
    }

    void *addr = mmap(NULL, sizeof(LockFreeRingBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return -1;
    }
    close(shm_fd);

    handle->rb = (LockFreeRingBuffer *)addr;
    if (create) {
        memset(handle->rb, 0, sizeof(LockFreeRingBuffer));
    }

    handle->semaphore = sem_open(sem_name, create ? O_CREAT : 0, S_IRUSR | S_IWUSR, 0);
    if (handle->semaphore == SEM_FAILED) {
        perror("sem_open");
        munmap(handle->rb, sizeof(LockFreeRingBuffer));
        return -1;
    }

    return 0;
}

void cleanup_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name) {
    if (handle->rb) {
        munmap(handle->rb, sizeof(LockFreeRingBuffer));
        handle->rb = NULL;
    }
    if (handle->semaphore) {
        sem_close(handle->semaphore);
        handle->semaphore = NULL;
    }
    sem_unlink(sem_name);
    shm_unlink(shm_name);
}

static inline int read_be32(const unsigned char *p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

char* offset_to_ptr(uint32_t offset) {
    return (char*)(&g_pool->data[0][0]) + offset;
}

uint32_t ptr_to_offset(const char* ptr) {
    return (uint32_t)(ptr - (char*)(&g_pool->data[0][0]));
}



bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length) {
    if (!data || length < 4 || length > SAMPLE_SIZE || !g_pool->data) return false;

    // 1) 범위 검사 (exclusive end)
    const unsigned char *pool_begin = &g_pool->data[0][0];
    const unsigned char *pool_limit = &g_pool->data[POOL_COUNT][0]; // exclusive

    if ((const unsigned char*)data < pool_begin || (const unsigned char*)data >= pool_limit) {
        size_t span = (size_t)(pool_limit - pool_begin);
        fprintf(stderr, "❌ [ENQ] OOR ptr=%p begin=%p limit=%p span=%zu SAMPLE_SIZE=%d POOL_COUNT=%d\n",
                data, pool_begin, pool_limit, span, SAMPLE_SIZE, POOL_COUNT);
        // 어디서 한 슬롯 밀렸는지 숫자로 확인
        ptrdiff_t diff = (const unsigned char*)data - pool_limit;
        fprintf(stderr, "👉 diff_from_limit=%td (should be < 0). slots_over=%td\n",
                diff, diff / SAMPLE_SIZE);
        return false;
    }

    // 2) 인덱스 역산 & 정렬 검사
    size_t off = (const unsigned char*)data - pool_begin;
    if (off % SAMPLE_SIZE != 0) {
        fprintf(stderr, "❌ [ENQ] misaligned ptr=%p off=%zu SAMPLE_SIZE=%d\n",
                data, off, SAMPLE_SIZE);
        return false;
    }
    uint32_t idx_dbg = off / SAMPLE_SIZE;
    if (idx_dbg >= POOL_COUNT) {
        fprintf(stderr, "❌ [ENQ] idx=%u >= POOL_COUNT=%u (off=%zu)\n",
                idx_dbg, POOL_COUNT, off);
        return false;
    }

    // 1) 예약
    uint32_t my = atomic_fetch_add_explicit(&rb->prod_resv, 1, memory_order_relaxed);
    // fprintf(stderr, "[ENQ] reserved slot=%u (prod_resv now=%u)\n", my, my + 1);

    // 2) 용량 확인
    for (int spin = 0;
        (int32_t)(my - atomic_load_explicit(&rb->cons_seq, memory_order_relaxed)) >= (int32_t)BUF_COUNT;
        ++spin) {
        cpu_relax();
    }

    // 3) 슬롯 쓰기
    uint32_t idx = my & (BUF_COUNT - 1);
    rb->offset[idx] = ptr_to_offset(data);

    // 4) 게시
    for (;;) {
        uint32_t pub = atomic_load_explicit(&rb->prod_pub, memory_order_relaxed);
        if ((int32_t)(my - pub) == 0) {
            if (atomic_compare_exchange_weak_explicit(
                &rb->prod_pub, &pub, my+1,
                memory_order_release, memory_order_relaxed)) break;
        } else {
            cpu_relax();
        }
    }
    return true;
}

bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length) {
    if (!g_pool) return false;

    void *pool_start = &g_pool->data[0][0];
    void *pool_end   = &g_pool->data[POOL_COUNT - 1][SAMPLE_SIZE - 1];
    
    for (;;) {
        uint32_t head = atomic_load_explicit(&rb->cons_seq, memory_order_relaxed);
        uint32_t tail = atomic_load_explicit(&rb->prod_pub, memory_order_acquire);

        if (head == tail) return false; // empty

        uint32_t new_head = head + 1;
        uint32_t exp = head;
        if (!atomic_compare_exchange_weak_explicit(
                &rb->cons_seq, &exp, new_head,
                memory_order_acquire, memory_order_relaxed)) {
            cpu_relax();
            continue; // 경쟁 → 재시도
        }

        uint32_t offset = rb->offset[head & (BUF_COUNT - 1)];
        const char *p = (const char *)pool_start + offset;

        // 범위 체크(잘못된 값이면 해제하지 말 것)
        if ((void*)p < pool_start || (void*)p > pool_end) {
            fprintf(stderr, "❌ [DEQ] pointer %p out of bounds [%p ~ %p]\n", p, pool_start, pool_end);
            return false;
        }

        int msg_len = read_be32((const unsigned char *)p);
        if (msg_len <= 0 || msg_len > SAMPLE_SIZE - 4) {
            fprintf(stderr, "❌ [DEQ] invalid message length: %d\n", msg_len);
            return false;
        }

        *out_ptr   = p + 4;
        *out_length = msg_len;
        // atomic_fetch_add_explicit(&deq_success_count, 1, memory_order_relaxed);
        return true;
    }
}
