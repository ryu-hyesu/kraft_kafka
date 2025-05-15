#include "shared_memory.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include "org_apache_kafka_clients_consumer_SharedMemoryConsumer.h"

static SharedMemoryHandle handle_req = {0};  // /cons_broker_shm
static SharedMemoryHandle handle_res = {0};  // /broker_cons_shm

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_consumer_SharedMemoryConsumer_writeSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (!handle_req.rb || !handle_req.semaphore) {
        if (initialize_shared_memory(&handle_req, "/broker_cons_shm", "/broker_cons_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (nativeBuffer == NULL) {
        fprintf(stderr, "ERROR: ByteBuffer is not direct or is NULL\n");
        return;
    }

    if (buffer_try_enqueue(handle_req.rb, (const char *)nativeBuffer, length)) {
        sem_post(handle_req.semaphore);
    }
}

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_consumer_SharedMemoryConsumer_writeSharedMemoryToServer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (!handle_res.rb || !handle_res.semaphore) {
        if (initialize_shared_memory(&handle_res, "/cons_broker_shm", "/cons_broker_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (nativeBuffer == NULL) {
        fprintf(stderr, "ERROR: ByteBuffer is not direct or is NULL\n");
        return;
    }

    if (buffer_try_enqueue(handle_res.rb, (const char *)nativeBuffer, length)) {
        sem_post(handle_res.semaphore);
    }
}

JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_consumer_SharedMemoryConsumer_readSharedMemoryByBuffer(JNIEnv *env, jobject obj) {
    if (!handle_req.rb || !handle_req.semaphore) {
        if (initialize_shared_memory(&handle_req, "/broker_cons_shm", "/broker_cons_sem", true) != 0) return NULL;
    }

    if (sem_trywait(handle_req.semaphore) == -1) return NULL;

    char buffer[BUF_SIZE];
    int length;
    if (!buffer_try_dequeue(handle_req.rb, buffer, &length)) return NULL;

    return (*env)->NewDirectByteBuffer(env, memcpy(malloc(length), buffer, length), length);
}

JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_consumer_SharedMemoryConsumer_readSharedMemoryByConsumer(JNIEnv *env, jobject obj) {
    if (!handle_res.rb || !handle_res.semaphore) {
        if (initialize_shared_memory(&handle_res, "/cons_broker_shm", "/cons_broker_sem", true) != 0) return NULL;
    }

    if (sem_trywait(handle_res.semaphore) == -1) return NULL;

    char buffer[BUF_SIZE];
    int length;
    if (!buffer_try_dequeue(handle_res.rb, buffer, &length)) return NULL;

    return (*env)->NewDirectByteBuffer(env, memcpy(malloc(length), buffer, length), length);
}
