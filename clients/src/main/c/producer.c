#include "shared_memory.h"
#include "shared_memory_pool.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

static SharedMemoryHandle handle = {0};

// producer 메모리 할당
JNIEXPORT jobject JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_allocateSharedMemoryByBuffer(JNIEnv *env, jobject obj){
    if (shm_pool == NULL) {
        if (init_shared_memory_pool() != 0)
            return NULL;
    }

    unsigned char* ptr = shm_pool_get();
    if (ptr == NULL) {
        fprintf(stderr, "❌ shm_pool_get() failed: no available slot.\n");
        return NULL;  // or throw exception
    }

    // create a DirectByteBuffer that wraps the native memory
    jobject buffer = (*env)->NewDirectByteBuffer(env, ptr, SAMPLE_SIZE);
    if (buffer == NULL) {
        fprintf(stderr, "❌ NewDirectByteBuffer failed\n");
        shm_pool_release(ptr);  // clean up if buffer creation failed
        return NULL;
    }

    return buffer;  // Java에서 ByteBuffer로 받게 됨
}

// 포인터를 받아 ringbuffer에 저장
JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_commitSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (shm_pool == NULL) {
        if (init_shared_memory_pool() != 0)
            return;
    }
    
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
    if (shm_pool == NULL) {
        if (init_shared_memory_pool() != 0)
            return NULL;
    }
    
    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return NULL;
    }

    if (sem_trywait(handle.semaphore) == -1) return NULL;

    const char *ptr;
    int length;
    if (!buffer_try_dequeue(handle.rb, &ptr, &length)) return NULL;

    return (*env)->NewDirectByteBuffer(env, (void*)ptr, length);

}
