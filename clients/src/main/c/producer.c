#include "shared_memory.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

static SharedMemoryHandle handle = {0};

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_writeSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) return;

    if (buffer_try_enqueue(handle.rb, (const char *)nativeBuffer, length)) {
        sem_post(handle.semaphore);
    }
}

JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByBuffer(JNIEnv *env, jobject obj) {
    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return NULL;
    }

    if (sem_trywait(handle.semaphore) == -1) return NULL;

    char buffer[BUF_SIZE];
    int length;
    if (!buffer_try_dequeue(handle.rb, buffer, &length)) return NULL;

    return (*env)->NewDirectByteBuffer(env, memcpy(malloc(length), buffer, length), length);
}
