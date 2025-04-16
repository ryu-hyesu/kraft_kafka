#include "shared_memory.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>    // epoll_create1, epoll_ctl, epoll_wait, struct epoll_event
#include <sys/eventfd.h>  // eventfd, EFD_NONBLOCK, EFD_SEMAPHORE
#include <unistd.h>       // read, write


int initialize_shared_memory(SharedMemoryHandle *handle, const char *shm_name, const char *sem_name, bool create) {
    printf("[INIT] Initializing shared memory: create=%d, shm_name=%s, sem_name=%s\n", create, shm_name, sem_name);

    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    int shm_fd = shm_open(shm_name, flags, S_IRUSR | S_IWUSR);
    if (shm_fd == -1) {
        perror("[INIT] shm_open failed");
        return -1;
    }
    printf("[INIT] shm_open succeeded: fd=%d\n", shm_fd);

    if (create && ftruncate(shm_fd, sizeof(LockFreeRingBuffer)) == -1) {
        perror("[INIT] ftruncate failed");
        close(shm_fd);
        return -1;
    }

    void *addr = mmap(NULL, sizeof(LockFreeRingBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("[INIT] mmap failed");
        close(shm_fd);
        return -1;
    }
    printf("[INIT] mmap succeeded, addr=%p\n", addr);
    close(shm_fd);

    handle->rb = (LockFreeRingBuffer *)addr;
    if (create) {
        memset(handle->rb, 0, sizeof(LockFreeRingBuffer));
        printf("[INIT] ring buffer zero-initialized\n");
    }

    handle->semaphore = sem_open(sem_name, create ? O_CREAT : 0, S_IRUSR | S_IWUSR, 0);
    if (handle->semaphore == SEM_FAILED) {
        perror("[INIT] sem_open failed");
        munmap(handle->rb, sizeof(LockFreeRingBuffer));
        return -1;
    }
    printf("[INIT] sem_open succeeded\n");

    handle->event_fd = eventfd(0, EFD_NONBLOCK);
    if (handle->event_fd == -1) {
        perror("[INIT] eventfd failed");
        return -1;
    }
    printf("[INIT] eventfd created: fd=%d\n", handle->event_fd);

    handle->epfd = epoll_create1(0);
    if (handle->epfd == -1) {
        perror("[INIT] epoll_create1 failed");
        close(handle->event_fd);
        return -1;
    }
    printf("[INIT] epoll created: fd=%d\n", handle->epfd);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = handle;

    if (epoll_ctl(handle->epfd, EPOLL_CTL_ADD, handle->event_fd, &ev) == -1) {
        perror("[INIT] epoll_ctl ADD failed");
        close(handle->event_fd);
        munmap(handle->rb, sizeof(LockFreeRingBuffer)); 
        return -1;
    }
    printf("[INIT] epoll_ctl registered eventfd\n");

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
    if (handle->event_fd != -1) {
        close(handle->event_fd);
        handle->event_fd = -1;
    }
    if (handle->epfd != -1) {
        close(handle->epfd);
        handle->epfd = -1;
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
