#include <jni.h>
#include <fcntl.h>      // O_CREAT, O_RDWR
#include <sys/mman.h>   // shm_open, mmap
#include <sys/stat.h>   // S_IRUSR, S_IWUSR
#include <unistd.h>     // ftruncate, close
#include <semaphore.h>  // sem_open, sem_post, sem_wait
#include <stdio.h>      // printf, snprintf
#include <stdint.h>     // uint64_t
#include <string.h>     // memset, strlen
#include "org_apache_kafka_clients_producer_SharedMemoryManager.h"


// Shared memory structure
#define BUF_COUNT 100
#define BUF_SIZE 1024
#define SHARED_MEMORY_NAME "/kafka_shared_memory" // POSIX 공유 메모리 이름
#define SEMAPHORE_NAME "/kafka_semaphore"        // POSIX 세마포어 이름

typedef struct {
    char data[BUF_COUNT][BUF_SIZE]; // Shared memory data buffer
    uint64_t prod_seq;              // Producer sequence
    uint64_t cons_seq;              // Consumer sequence
} shm_mem_t;

// Global shared memory pointer
shm_mem_t *shm_base = NULL;
sem_t *semaphore = NULL;

// Initialize shared memory and semaphore
int initialize_shared_memory() {
    // Create or open shared memory
    int shm_fd = shm_open(SHARED_MEMORY_NAME, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd == -1) {
        perror("Error: Failed to open shared memory");
        return -1;
    }

    // Set the size of the shared memory
    if (ftruncate(shm_fd, sizeof(shm_mem_t)) == -1) {
        perror("Error: Failed to set shared memory size");
        close(shm_fd);
        return -1;
    }

    // Map the shared memory into the process address space
    shm_base = (shm_mem_t *)mmap(NULL, sizeof(shm_mem_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_base == MAP_FAILED) {
        perror("Error: Failed to map shared memory");
        close(shm_fd);
        return -1;
    }
    close(shm_fd); // Shared memory file descriptor no longer needed

    // Initialize the shared memory content (first-time creation only)
    if (shm_base->prod_seq == 0 && shm_base->cons_seq == 0) {
        memset(shm_base->data, 0, sizeof(shm_base->data));
        shm_base->prod_seq = 0;
        shm_base->cons_seq = 0;
    }

    // Create or open a semaphore for synchronization
    semaphore = sem_open(SEMAPHORE_NAME, O_CREAT, S_IRUSR | S_IWUSR, 0);
    if (semaphore == SEM_FAILED) {
        perror("Error: Failed to open semaphore");
        munmap(shm_base, sizeof(shm_mem_t));
        return -1;
    }

    return 0;
}

// Cleanup shared memory and semaphore
void cleanup_shared_memory() {
    if (shm_base) {
        munmap(shm_base, sizeof(shm_mem_t));
    }
    if (semaphore) {
        sem_close(semaphore);
    }
    sem_unlink(SEMAPHORE_NAME);
    shm_unlink(SHARED_MEMORY_NAME);
}

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryManager_writeSharedMemoryByBuffer
(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (!buffer) {
        printf("Error: Buffer is null.\n");
        return;
    }

    // Direct ByteBuffer의 native 주소 가져오기
    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) {
        printf("Error: Failed to get direct buffer address. Native buffer is null.\n");
        return;
    }

    // 유효한 길이인지 검사
    if (length <= 0 || length > BUF_SIZE) {
        printf("Error: Invalid buffer length: %d, BUF_SIZE: %d\n", length, BUF_SIZE);
        return;
    }

    if (!shm_base) {
        if (initialize_shared_memory() != 0) {
            printf("Error: Failed to initialize shared memory.\n");
            return;
        }
    }

    // Remove first 4 bytes and get actual data
    char *data = (char *)nativeBuffer + 4;
    int data_length = length - 4;

    // Get write index
    uint64_t index = shm_base->prod_seq % BUF_COUNT;

    // Save data size at the beginning of the buffer
    memcpy(shm_base->data[index], &data_length, sizeof(int));

    // Copy actual data after the size metadata
    memcpy(shm_base->data[index] + sizeof(int), data, data_length);

    // 생산자 시퀀스 증가
    shm_base->prod_seq++;

    // 소비자 알림
    if (sem_post(semaphore) == -1) {
        perror("sem_post failed");
    }
}

JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryManager_readSharedMemoryByBuffer
(JNIEnv *env, jobject obj) {
    if (!shm_base) {
        if (initialize_shared_memory() != 0) {
            printf("Failed to initialize shared memory.\n");
            return NULL;
        }
    }

    // ✅ Non-blocking 방식 적용: 데이터가 없으면 즉시 return
    if (sem_trywait(semaphore) == -1) {
        return NULL;  // 데이터가 없으면 대기하지 않고 즉시 반환
    }

    if (shm_base->cons_seq >= shm_base->prod_seq) {
        return NULL;
    }

    // Get read index
    uint64_t index = shm_base->cons_seq % BUF_COUNT;

    // Read data size
    int data_length;
    memcpy(&data_length, shm_base->data[index], sizeof(int));

    // Allocate Java ByteBuffer
    jobject byteBuffer = (*env)->NewDirectByteBuffer(env, shm_base->data[index] + sizeof(int), data_length);

    shm_base->cons_seq++;

    return byteBuffer;
}