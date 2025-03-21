#include <jni.h>
#include <fcntl.h>      // O_CREAT, O_RDWR
#include <sys/mman.h>   // shm_open, mmap
#include <sys/stat.h>   // S_IRUSR, S_IWUSR
#include <unistd.h>     // ftruncate, close
#include <semaphore.h>  // sem_open, sem_post, sem_wait, sem_trywait
#include <stdio.h>      // printf, perror
#include <stdint.h>     // uint64_t, int types
#include <string.h>     // memset, memcpy
#include <stdatomic.h>  // atomic operations
#include <stdbool.h>    // bool type
#include "org_apache_kafka_clients_producer_SharedMemoryManager.h"  // JNI header

// Shared memory structure
#define BUF_COUNT 100
#define BUF_SIZE 1024
#define SHARED_MEMORY_NAME "/kafka_shared_memory" // POSIX 공유 메모리 이름
#define SEMAPHORE_NAME "/kafka_semaphore"        // POSIX 세마포어 이름

typedef struct {
    char data[BUF_COUNT][BUF_SIZE]; // Shared memory data buffer
    atomic_uint_fast64_t prod_seq;  // Producer sequence (tail)
    atomic_uint_fast64_t cons_seq;  // Consumer sequence (head)
} LockFreeRingBuffer;

// Global shared memory pointer
static LockFreeRingBuffer *rb = NULL;
sem_t *semaphore = NULL;

// Initialize shared memory and semaphore
int initialize_shared_memory() {
    int shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd == -1) {
        perror("Error: Failed to open shared memory");
        return -1;
    }

    if (ftruncate(shm_fd, sizeof(LockFreeRingBuffer)) == -1) {
        perror("Error: Failed to set shared memory size");
        close(shm_fd);
        shm_unlink(SHARED_MEMORY_NAME);
        return -1;
    }

    rb = (LockFreeRingBuffer *)mmap(NULL, sizeof(LockFreeRingBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (rb == MAP_FAILED) {
        perror("Error: Failed to map shared memory");
        close(shm_fd);
        shm_unlink(SHARED_MEMORY_NAME);
        return -1;
    }
    close(shm_fd);

    // Shared memory 초기화 (최초 실행 시)
    memset(rb, 0, sizeof(LockFreeRingBuffer));

    // 세마포어 생성 또는 열기
    semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, S_IRUSR | S_IWUSR, 0);
    if (semaphore == SEM_FAILED) {
        perror("Error: Failed to open semaphore");
        munmap(rb, sizeof(LockFreeRingBuffer));
        shm_unlink(SHARED_MEMORY_NAME);
        return -1;
    }

    return 0;
}

// Cleanup shared memory and semaphore
void cleanup_shared_memory() {
    if (rb) {
        munmap(rb, sizeof(LockFreeRingBuffer));
    }
    if (semaphore) {
        sem_close(semaphore);
    }
    sem_unlink(SEMAPHORE_NAME);
    shm_unlink(SHARED_MEMORY_NAME);
}

// WRITE (enqueue)
bool buffer_try_enqueue(LockFreeRingBuffer *rb, const char *data, int length) {
    uint64_t tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);

    if ((tail + 1) % BUF_COUNT == head) {
        return false;  // 버퍼가 가득 참
    }

    const char *actual_data = data + 4;
    int actual_length = length - 4;

    memcpy(rb->data[tail % BUF_COUNT], &actual_length, sizeof(int));
    memcpy(rb->data[tail % BUF_COUNT] + sizeof(int), actual_data, actual_length);

    atomic_thread_fence(memory_order_release); // 현재 스레드가 수행한 모든 이전 쓰기 연산이 완료되었을 때 연산이 실행됨
    atomic_fetch_add_explicit(&rb->prod_seq, 1, memory_order_release); // memory_order-release 현재 스레드가 쓰기 연산을 먼저 수행하도록 보장함 & 다른 스레드(memory_order_acquire)가 읽을 때 이 값이 최신 상태임을 보장함

    return true;
}

// Java -> C (Write)
JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryManager_writeSharedMemoryByBuffer
(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (!buffer) return;

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) return;

    if (!rb && initialize_shared_memory() != 0) return;

    if (!buffer_try_enqueue(rb, (const char *)nativeBuffer, length)) return;

    sem_post(semaphore); // 읽기 시 세마포어에 알림 전송
}

// Java -> C (Read)
JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryManager_readSharedMemoryByBuffer
(JNIEnv *env, jobject obj) {
    if (!rb && initialize_shared_memory() != 0) return NULL;

    // 세마포어를 통해 현재 쓰기 작업이 이루어지지 않았음을 확인
    if (sem_trywait(semaphore) == -1) {
        return NULL;
    }

    uint64_t head = atomic_load_explicit(&rb->cons_seq, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&rb->prod_seq, memory_order_acquire); // 현재 스레드가 읽기 연산을 먼저 수행하도록 보장한다.

    if (head == tail) {
        sem_post(semaphore);
        return NULL;
    }

    uint64_t index = head % BUF_COUNT;

    int data_length;
    memcpy(&data_length, rb->data[index], sizeof(int));

    jobject byteBuffer = (*env)->NewDirectByteBuffer(env, rb->data[index] + sizeof(int), data_length);
    if (!byteBuffer) {
        sem_post(semaphore);
        return NULL;
    }

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&rb->cons_seq, head + 1, memory_order_release);

    return byteBuffer;
}
