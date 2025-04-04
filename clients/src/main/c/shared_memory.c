#include "shared_memory.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>


LockFreeRingBuffer *rb = NULL;
sem_t *semaphore = NULL;

int initialize_shared_memory(const char *shm_name, const char *sem_name, bool create) {
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

    rb = mmap(NULL, sizeof(LockFreeRingBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (rb == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return -1;
    }
    close(shm_fd);

    if (create) {
        memset(rb, 0, sizeof(LockFreeRingBuffer));
    }

    semaphore = sem_open(sem_name, create ? O_CREAT : 0, S_IRUSR | S_IWUSR, 0);
    if (semaphore == SEM_FAILED) {
        perror("sem_open");
        munmap(rb, sizeof(LockFreeRingBuffer));
        return -1;
    }

    return 0;
}

void cleanup_shared_memory(const char *shm_name, const char *sem_name) {
    if (rb) {
        munmap(rb, sizeof(LockFreeRingBuffer));
        rb = NULL;
    }
    if (semaphore) {
        sem_close(semaphore);
        semaphore = NULL;
    }
    sem_unlink(sem_name);
    shm_unlink(shm_name);
}

bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length) {
    uint64_t tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);

    if ((tail + 1) % BUF_COUNT == head) {
        return false; // full
    }

    const char *actual_data = data + 4;
    int actual_length = length - 4;

    memcpy(rb->data[tail % BUF_COUNT], &actual_length, sizeof(int));
    memcpy(rb->data[tail % BUF_COUNT] + sizeof(int), actual_data, actual_length);

    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&rb->prod_seq, 1, memory_order_release);

    return true;
}

bool buffer_try_dequeue(LockFreeRingBuffer *rb, char *out, int *out_length) {
    uint64_t head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire);

    if (head == tail) {
        return false; // empty
    }

    uint64_t index = head % BUF_COUNT;
    memcpy(out_length, rb->data[index], sizeof(int));
    memcpy(out, rb->data[index] + sizeof(int), *out_length);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&rb->cons_seq, head + 1, memory_order_release);
    return true;
}
