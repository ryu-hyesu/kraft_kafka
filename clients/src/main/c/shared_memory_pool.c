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

unsigned char (*shm_pool)[SAMPLE_SIZE];  // 포인터 to array-of-SAMPLE_SIZE
static shm_memory_pool freelist;

int init_shared_memory_pool() {
    size_t shm_size = POOL_COUNT * SAMPLE_SIZE;

    int fd = shm_open("/shm_pool_region", O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        // fprintf(stderr, "[SHM][ERROR] shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, shm_size) == -1) {
        // fprintf(stderr, "[SHM][ERROR] ftruncate failed (size: %zu): %s\n", shm_size, strerror(errno));
        close(fd);
        return -1;
    }

    void *addr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        // fprintf(stderr, "[SHM][ERROR] mmap failed (size: %zu): %s\n", shm_size, strerror(errno));
        close(fd);
        return -1;
    }

    shm_pool = addr;
    // fprintf(stderr, "[SHM] mmap succeeded: address = %p\n", shm_pool);
    close(fd);

    // freelist 초기화
    atomic_store_explicit(&freelist.head, POOL_COUNT, memory_order_relaxed);
    atomic_store_explicit(&freelist.tail, 0, memory_order_relaxed);

    for (uint32_t i = 0; i < POOL_COUNT; ++i) {
        freelist.slots[i] = i;
    }

    return 0;
}

unsigned char* shm_pool_get() {
    uint32_t tail, head, idx;
    int attempt = 0;

    while (1) {
        tail = atomic_load_explicit(&freelist.tail, memory_order_relaxed);
        head = atomic_load_explicit(&freelist.head, memory_order_relaxed);

        if (tail == head) {
            // fprintf(stderr, "[SHM_POOL_GET][EMPTY] attempt=%d → tail == head == %u → pool empty\n", attempt, tail);
            return NULL;
        }

        idx = freelist.slots[tail & (POOL_COUNT - 1)];
        uint32_t new_tail = (tail + 1) & ((2 * POOL_COUNT) - 1);

        if (atomic_compare_exchange_weak_explicit(
                &freelist.tail, &tail, new_tail,
                memory_order_release, memory_order_relaxed)) {

            return &shm_pool[idx][0];
        } else {
            // fprintf(stderr, "[SHM_POOL_GET][RETRY] attempt=%d → CAS failed → tail(now)=%u, head=%u\n", attempt, tail, head);
        }

        attempt++;
        if (attempt > 1000) {
            // fprintf(stderr, "[SHM_POOL_GET][FAILSAFE] attempt > 1000 → something may be wrong, aborting\n");
            return NULL;
        }
    }
}


void shm_pool_release(unsigned char* ptr) {
    if ((uintptr_t)ptr < (uintptr_t)&shm_pool[0][0] ||
        (uintptr_t)ptr >= (uintptr_t)&shm_pool[POOL_COUNT][0]) {
        // fprintf(stderr, "ERROR: Invalid pointer passed to shm_pool_release\n");
        return;
    }

    ptrdiff_t offset = ptr - &shm_pool[0][0];
    if (offset % SAMPLE_SIZE != 0) {
        // fprintf(stderr, "ERROR: Misaligned pointer passed to shm_pool_release\n");
        return;
    }

    uint32_t idx = offset / SAMPLE_SIZE;
    uint32_t head;

    while (1) {
        head = atomic_load_explicit(&freelist.head, memory_order_relaxed);
        uint32_t new_head = (head + 1) & ((2 * POOL_COUNT) - 1);

        freelist.slots[head & (POOL_COUNT - 1)] = idx;

        if (atomic_compare_exchange_weak_explicit(
                &freelist.head, &head, new_head,
                memory_order_release, memory_order_relaxed)) {
            return;
        }
    }
}
