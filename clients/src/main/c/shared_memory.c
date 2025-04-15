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

    // event_fd 생성성
    handle->event_fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (handle->event_fd == -1) { perror("eventfd"); return -1; }

    // epoll fd 생성
    int epfd = epoll_create1(0);

    // epoll fd에 등록할 이벤트 생성
    struct epoll_event ev;
    ev.events = EPOLLIN; // 읽을 수 있는 데이터 도착 시 감지
    ev.data.ptr = handle;

    // epoll fd에 event_fd 등록
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, handle->event_fd, &ev) == -1) {
        perror("epoll_ctl ADD");
        close(handle->event_fd);
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
    if (handle->event_fd != -1) {
        close(handle->event_fd);
        handle->event_fd = -1;
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
