#include "shared_memory.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

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

        atomic_store_explicit(&handle->rb->prod_seq, 0, memory_order_relaxed);
        atomic_store_explicit(&handle->rb->cons_seq, 0, memory_order_relaxed);
        for (uint64_t i = 0; i < BUF_COUNT; ++i) {
            atomic_store_explicit(&handle->rb->buf[i].seq, i, memory_order_relaxed);
        }
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

bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length) {
    if (length < 4) return false;

    const char *actual_data = data + 4;
    int actual_length = length - 4;
    if (actual_length <= 0 || actual_length > BUF_SIZE - sizeof(int)) {
        fprintf(stderr, "[SHM] ERROR: Invalid enqueue length=%d (actual=%d)\n", length, actual_length);
        return false;
    }

    while (1) {
        uint64_t seq = atomic_load_explicit(&rb->prod_seq, memory_order_relaxed);
        uint64_t index = seq % BUF_COUNT;
        Buf *slot = &rb->buf[index];

        // 반드시 최신 slot_seq 읽기
        uint64_t slot_seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = (int64_t)slot_seq - (int64_t)seq;

        if (diff == 0) {
            // producer slot 예약
            uint64_t expected = seq;
            if (!atomic_compare_exchange_strong_explicit(&rb->prod_seq, &expected, seq + 1,
                                                         memory_order_acquire, memory_order_relaxed)) {
                continue; // 실패하면 다음 loop에서 seq를 다시 읽는다!
            }

            // 🧠 안전하게 슬롯 확보 후에만 write 시작
            memset(slot->data, 0, BUF_SIZE);
            memcpy(slot->data, &actual_length, sizeof(int));
            memcpy(slot->data + sizeof(int), actual_data, actual_length);

            atomic_thread_fence(memory_order_release); // 모든 write 완료

            // slot 사용 완료 알림
            atomic_store_explicit(&slot->seq, seq + 1, memory_order_release);
            return true;
        } else if (diff < 0) {
            return false;  // 아직 소비가 안된 슬롯
        } else {
            __builtin_ia32_pause();
        }
    }
}


bool buffer_try_dequeue(LockFreeRingBuffer *rb, char *out, int *out_length) {
    while (1) {
        uint64_t seq = atomic_load_explicit(&rb->cons_seq, memory_order_relaxed);
        uint64_t index = seq % BUF_COUNT;
        Buf *slot = &rb->buf[index];

        uint64_t slot_seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        int64_t diff = (int64_t)slot_seq - (int64_t)(seq + 1);

        if (diff == 0) {
            uint64_t expected = seq;
            if (atomic_compare_exchange_strong_explicit(&rb->cons_seq, &expected, seq + 1,
                                                        memory_order_acquire, memory_order_relaxed)) {
                // 데이터 읽기 전 보호
                atomic_thread_fence(memory_order_acquire);

                int actual_length;
                memcpy(&actual_length, slot->data, sizeof(int));

                if (actual_length <= 0 || actual_length > BUF_SIZE - sizeof(int)) {
                    fprintf(stderr, "[SHM] ERROR: Invalid dequeue actual_length=%d\n", actual_length);

                    continue;
                }

                memcpy(out, slot->data + sizeof(int), actual_length);
                *out_length = actual_length;

                // flush 후 재사용 가능 알림
                atomic_thread_fence(memory_order_release);
                atomic_store_explicit(&slot->seq, seq + BUF_COUNT, memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            // 아직 쓰여지지 않은 슬롯
            return false;
        } else {
            // 다른 consumer가 먼저 처리할 차례
            __builtin_ia32_pause();
        }
    }
}
