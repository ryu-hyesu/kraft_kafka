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
    return (char*)(&shm_pool[0][0]) + offset;
}

uint32_t ptr_to_offset(const char* ptr) {
    return (uint32_t)(ptr - (char*)(&shm_pool[0][0]));
}


bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length) {
    uint64_t head, tail;

    if (!data) {
        fprintf(stderr, "❌ [ENQ] data pointer is NULL\n");
        return false;
    }
    if (length < 4 || length > SAMPLE_SIZE) {
        fprintf(stderr, "❌ [ENQ] invalid length: %d\n", length);
        return false;
    }

    if (!shm_pool) {
        fprintf(stderr, "❌ [ENQ] shm_pool is NULL (not initialized)\n");
        return false;
    }

    void *pool_start = &shm_pool[0][0];
    void *pool_end = &shm_pool[POOL_COUNT - 1][SAMPLE_SIZE - 1];
    // fprintf(stderr, "[ENQ] pointer %p out of shm_pool bounds [%p ~ %p]\n", data, pool_start, pool_end);
    if ((void *)data < pool_start || (void *)data > pool_end) {
        fprintf(stderr, "❌ [ENQ] pointer %p out of shm_pool bounds [%p ~ %p]\n", data, pool_start, pool_end);
        return false;
    }


    while (true) {
        tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire);
        head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);
        if ((tail + 1) % BUF_COUNT == head)
            return false; // full

        // fprintf(stderr, "✅ [ENQ] enqueueing pointer: %p (length: %d)\n", data, length);
        // rb->data[tail % BUF_COUNT] = data;
        uint32_t offset = ptr_to_offset(data);
        // fprintf(stderr, "✅ [ENQ] enqueueing pointer: %p (offset: %u, length: %d)\n", data, offset, length);
        rb->offset[tail % BUF_COUNT] = offset;

        atomic_thread_fence(memory_order_release);
        atomic_store_explicit(&rb->prod_seq, tail + 1, memory_order_release);
        return true;
    }
}

bool buffer_try_dequeue(LockFreeRingBuffer *rb, const char **out_ptr, int *out_length) {
    uint64_t head, tail;

    if (!shm_pool) {
        fprintf(stderr, "❌ [DEQ] shm_pool is NULL (not initialized)\n");
        return false;
    }

    void *pool_start = &shm_pool[0][0];
    void *pool_end = &shm_pool[POOL_COUNT - 1][SAMPLE_SIZE - 1];

    // offset → ptr 변환 함수
    uint32_t offset;
    const char *p;

    while (true) {
        head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);
        tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire);
        if (head == tail)
            return false; // empty

        offset = rb->offset[head % BUF_COUNT];
        p = (const char *)pool_start + offset;

        if (!p) {
            fprintf(stderr, "❌ [DEQ] null pointer reconstructed from offset %u\n", offset);
            return false;
        }

        if ((void *)p < pool_start || (void *)p > pool_end) {
            fprintf(stderr, "❌ [DEQ] pointer %p out of shm_pool bounds [%p ~ %p]\n", p, pool_start, pool_end);
            return false;
        }

        // fprintf(stderr, "✅ [DEQ] Dequeue ptr = %p (offset = %u)\n", p, offset);
        // fprintf(stderr, "    first 4 bytes = %02X %02X %02X %02X\n",
                // p[0], p[1], p[2], p[3]);

        int msg_len = read_be32((const unsigned char *)p);

        if (msg_len <= 0 || msg_len > SAMPLE_SIZE - 4) {
            fprintf(stderr, "❌ [DEQ] invalid message length: %d\n", msg_len);
            return false;
        }

        *out_ptr = p + 4;
        *out_length = msg_len;

        atomic_thread_fence(memory_order_release);
        atomic_store_explicit(&rb->cons_seq, head + 1, memory_order_release);
        return true;
    }
}

