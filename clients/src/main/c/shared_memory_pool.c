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

// shm_pool 크기와 SAMPLE_SIZE는 미리 정의되어 있어야 합니다.
unsigned char (*shm_pool)[SAMPLE_SIZE] = NULL;  // ✅ 포인터 to array-of-SAMPLE_SIZE
uint8_t in_use[POOL_COUNT] = {0};

int init_shared_memory_pool() {
    size_t shm_size = POOL_COUNT * SAMPLE_SIZE;
    // fprintf(stderr, "[SHM] Attempting to allocate shared memory of size: %zu bytes\n", shm_size);

    int fd = shm_open("/shm_pool_region", O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        fprintf(stderr, "[SHM][ERROR] shm_open failed: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, shm_size) == -1) {
        fprintf(stderr, "[SHM][ERROR] ftruncate failed (size: %zu): %s\n", shm_size, strerror(errno));
        close(fd);
        return -1;
    }

    void *addr = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "[SHM][ERROR] mmap failed (size: %zu): %s\n", shm_size, strerror(errno));
        close(fd);
        return -1;
    }

    shm_pool = addr;
    fprintf(stderr, "[SHM] mmap succeeded: address = %p\n", shm_pool);

    close(fd);
    return 0;
}

unsigned char* shm_pool_get() {
    for (int i = 0; i < POOL_COUNT; ++i) {
        if (__sync_bool_compare_and_swap(&in_use[i], 0, 1)) {
            // fprintf(stderr, "✅ shm_pool_get(): index %d → %p\n", i, shm_pool[i]);
            return shm_pool[i];
        }
    }
    fprintf(stderr, "❌ shm_pool_get() failed: pool exhausted\n");
    return NULL;
}

void shm_pool_release(unsigned char* ptr) {
    // ptr이 shm_pool 내의 유효한 포인터인지 확인
    if (ptr < (unsigned char*)shm_pool || ptr >= (unsigned char*)shm_pool + POOL_COUNT * SAMPLE_SIZE) {
        fprintf(stderr, "ERROR: Invalid pointer passed to shm_pool_release\n");
        return;
    }

    // ptr이 shm_pool 내에 있는 포인터라면, 이를 적절히 해제
    ptrdiff_t offset = ptr - &shm_pool[0][0];
    int idx = offset / SAMPLE_SIZE;

    if (idx >= 0 && idx < POOL_COUNT) {
        in_use[idx] = 0;
    } else {
        fprintf(stderr, "ERROR: Invalid index in shm_pool_release: idx = %d\n", idx);
    }
}
