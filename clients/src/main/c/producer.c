#include "shared_memory.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>      // read(), write()
#include <sys/epoll.h>   // epoll_create, epoll_ctl, epoll_wait
#include <stdint.h>      // uint64_t
#include <stdio.h>       // perror, printf 등
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

static SharedMemoryHandle handle = {0};

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_writeSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    printf("[WRITE] using eventfd=%d, handle ptr=%p\n", handle.event_fd, &handle);

    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) return;

    printf("[WRITE] buffer_try_enqueue: length=%d\n", length);
    if (buffer_try_enqueue(handle.rb, (const char *)nativeBuffer, length)) {
        printf("[WRITE] enqueue success, signaling eventfd\n");
        uint64_t val = 1;
        if (write(handle.event_fd, &val, sizeof(val)) != sizeof(val)) {
            perror("[WRITE] eventfd write failed");
        }
    } else {
        printf("[WRITE] enqueue failed: ring buffer full\n");
    }
}

JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByBuffer(JNIEnv *env, jobject obj) {
    printf("[WRITE] using eventfd=%d, handle ptr=%p\n", handle.event_fd, &handle);

    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return NULL;
    }

    // if (sem_trywait(handle.semaphore) == -1) return NULL;
    printf("[READ] Waiting for epoll...\n");
    fflush(stdout);
    struct epoll_event events[1];
    // int ready = epoll_wait(handle.epfd, events, 1, -1);
    int ready = 1;
    if (ready <= 0) {
        perror("[READ] epoll_wait failed or timed out");
        return NULL;
    }
    printf("[READ] epoll signaled\n");

    uint64_t val;
    if (read(handle.event_fd, &val, sizeof(val)) == -1) {
        perror("[READ] read eventfd failed");
        return NULL;
    }
    printf("[READ] eventfd read, val=%lu\n", val);

    char buffer[BUF_SIZE];
    int length;
    if (!buffer_try_dequeue(handle.rb, buffer, &length)) {
        printf("[READ] dequeue failed: buffer empty\n");
        return NULL;
    }
    printf("[READ] dequeue success, length=%d\n", length);

    return (*env)->NewDirectByteBuffer(env, memcpy(malloc(length), buffer, length), length);
}
