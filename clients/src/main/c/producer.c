// producer.c


#include "shared_memory.h"
#include "shared_memory_pool.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>   // getpid
#include <pthread.h>
#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

// counter
_Atomic uint64_t g_bk_relax_tot = 0, g_bk_yield_tot = 0, g_bk_nanos_tot = 0;
_Atomic uint64_t g_bk_yield_ns_tot = 0, g_bk_nanos_ns_tot = 0;

_Atomic uint64_t g_enq_total_ns = 0;
_Atomic uint64_t g_enq_cnt = 0;
_Atomic uint64_t g_enq_cap_wait_ns = 0;    
_Atomic uint64_t g_enq_pub_cas_wait_ns = 0;

_Atomic uint64_t g_deq_total_ns = 0;
_Atomic uint64_t g_deq_cnt = 0;
_Atomic uint64_t g_deq_cas_wait_ns = 0;

static SharedMemoryHandle handle = {0};

// big buffer : 한 번만 NewDirectByteBuffer 생성
static jobject g_pool_bigbuf_global = NULL; // GlobalRef to DirectByteBuffer wrapping entire pool
static void*   g_pool_base = NULL;
static jlong   g_pool_bytes = 0;

// pool 없음 검사 매크로
#define ENSURE_POOL_OR_RETURN(env, ret) do { if (!g_pool) { if (init_shared_memory_pool() != 0) return (ret); } } while(0)

__attribute__((constructor))
static void shm_ctor(void) {
    setvbuf(stderr, NULL, _IONBF, 0); // stderr 무버퍼
    fprintf(stderr, "[SHM][ctor] libsharedmemory loaded (pid=%d)\n", getpid());
}

JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_allocateSharedMemoryByBuffer
  (JNIEnv *env, jobject obj)
{
    ENSURE_POOL_OR_RETURN(env, NULL);

    unsigned char* ptr = shm_pool_get();
    if (!ptr) {
        fprintf(stderr, "❌ shm_pool_get() failed: no available slot.\n");
        return NULL;
    }

    // 매핑 가능성 확인
    volatile unsigned char touch0 = ptr[0];
    volatile unsigned char touchN = ptr[SAMPLE_SIZE - 1];
    (void)touch0; (void)touchN;

    (*env)->ExceptionClear(env);
    jobject buffer = (*env)->NewDirectByteBuffer(env, (void*)ptr, (jlong)SAMPLE_SIZE);
    if ((*env)->ExceptionCheck(env)) {
        fprintf(stderr, "❌ Exception in NewDirectByteBuffer (ptr=%p size=%d)\n", (void*)ptr, SAMPLE_SIZE);
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }

    if (buffer == NULL) {
        fprintf(stderr, "❌ NewDirectByteBuffer returned NULL (ptr=%p size=%d)\n", (void*)ptr, SAMPLE_SIZE);
        shm_pool_release(ptr);
        return NULL;
    }

    return buffer;
}

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_releaseSharedmemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer) {
    if (!buffer) return;
    void *addr = (*env)->GetDirectBufferAddress(env, buffer);
    if(!addr) return;
    unsigned char *base = (unsigned char*)addr;
    shm_pool_release(base);
}

JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_getPoolBigBuffer(
    JNIEnv *env, jobject obj)
{
    ENSURE_POOL_OR_RETURN(env, NULL);

    if (!g_pool_base) {
        g_pool_base  = (void*)&g_pool->data[0][0];
        g_pool_bytes = (jlong)((jlong)POOL_COUNT * (jlong)SAMPLE_SIZE);
    }

    if (g_pool_bigbuf_global) {
        // 이미 GlobalRef가 있으면 로컬 참조로 반환
        return (*env)->NewLocalRef(env, g_pool_bigbuf_global);
    }

    jobject big = (*env)->NewDirectByteBuffer(env, g_pool_base, g_pool_bytes);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!big) return NULL;

    g_pool_bigbuf_global = (*env)->NewGlobalRef(env, big);
    // 반환은 로컬 ref (big)로 함
    return big;
}

// 인덱스 할당
JNIEXPORT jint JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_allocateSharedMemoryIndex(
    JNIEnv *env, jobject obj)
{
    ENSURE_POOL_OR_RETURN(env, -1);

    unsigned char* ptr = shm_pool_get();
    if (!ptr) return -1;

    uintptr_t base = (uintptr_t)&g_pool->data[0][0];
    uintptr_t p    = (uintptr_t)ptr;
    uint32_t idx   = (uint32_t)((p - base) / (uintptr_t)SAMPLE_SIZE);

    return (jint)idx;
}

// 슬롯 인덱스로 커밋 (버퍼 객체는 Java 쪽에서 보유)
JNIEXPORT jboolean JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_commitSharedMemoryByIndex(
    JNIEnv *env, jobject obj, jint jidx, jint length)
{
    ENSURE_POOL_OR_RETURN(env, JNI_FALSE);

    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return JNI_FALSE;
    }

    if (jidx < 0 || jidx >= (jint)POOL_COUNT) return JNI_FALSE;

    unsigned char* base = &g_pool->data[0][0];
    unsigned char* slot = base + ((size_t)jidx * (size_t)SAMPLE_SIZE);

    if (buffer_try_enqueue(handle.rb, (const char*)slot, length)) {
        atomic_fetch_add_explicit(&enq_success_count, 1, memory_order_relaxed);
        sem_post(handle.semaphore);
        return JNI_TRUE;
    } else {
        // 커밋 실패 시 즉시 반납할 수 있음
        // shm_pool_release(slot);
        return JNI_FALSE;
    }
}

// 명시적 인덱스 반납
JNIEXPORT void JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_releaseSharedMemoryIndex(
    JNIEnv *env, jobject obj, jint jidx)
{
    if (!g_pool) return;
    if (jidx < 0 || jidx >= (jint)POOL_COUNT) return;
    unsigned char* base = &g_pool->data[0][0];
    unsigned char* slot = base + ((size_t)jidx * (size_t)SAMPLE_SIZE);
    shm_pool_release(slot);
}

// buffer를 받아서 enqueue
JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_commitSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    if (g_pool == NULL) {
        if (init_shared_memory_pool() != 0)
            return;
    }
    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) return;

    if (buffer_try_enqueue(handle.rb, (const char *)nativeBuffer, length)) {
        atomic_fetch_add_explicit(&enq_success_count, 1, memory_order_relaxed);
        sem_post(handle.semaphore);
    } else {
        fprintf(stderr, "fail to msg\n");
    }
}

JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByIndex(JNIEnv *env, jobject obj) {
    ENSURE_POOL_OR_RETURN(env, NULL);

    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0)
            return NULL;
    }

    if (sem_trywait(handle.semaphore) == -1) {
        if (errno != EAGAIN) {
            perror("[SHM] sem_trywait failed");
        }
        return NULL;
    }

    const char *ptr = NULL;
    int length = 0;
    if (!buffer_try_dequeue(handle.rb, &ptr, &length)) {
        fprintf(stderr, "[SHM] dequeue failed\n");
        return NULL;
    }

    uintptr_t base = (uintptr_t)&g_pool->data[0][0];
    uintptr_t p = (uintptr_t)ptr - 4;
    int index = (int)((p - base) / SAMPLE_SIZE);

    jobject buffer = (*env)->NewDirectByteBuffer(env, (void*)ptr, length);
    if (!buffer) {
        shm_pool_release((void*)p);
        return NULL;
    }

    return buffer;
}


// consumer
JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByBuffer(
    JNIEnv *env, jobject obj)
{
    pid_t pid = getpid();
    unsigned long tid = (unsigned long)pthread_self();

    ENSURE_POOL_OR_RETURN(env, NULL);

    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) {
            fprintf(stderr, "[JNI][%d:%lu] ❌ initialize_shared_memory failed\n", pid, tid);
            return NULL;
        }
    }

    if (sem_trywait(handle.semaphore) == -1) {
        int e = errno;
        if (e != EAGAIN) {
            fprintf(stderr, "[JNI][%d:%lu] ⚠️ sem_trywait error: %s\n", pid, tid, strerror(e));
        }
        return NULL;
    }

    const char *ptr = NULL;
    int length = 0;
    if (!buffer_try_dequeue(handle.rb, &ptr, &length)) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ dequeue failed after sem_trywait; length=%d ptr=%p\n", pid, tid, length, (void*)ptr);
        return NULL;
    }

    unsigned char *base = (unsigned char*)ptr - 4;

    void *copy = malloc((size_t)length);
    if (!copy) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ malloc(%d) failed\n", pid, tid, length);
        shm_pool_release(base);
        return NULL;
    }
    memcpy(copy, ptr, (size_t)length);
    shm_pool_release(base);
    atomic_fetch_add_explicit(&deq_success_count, 1, memory_order_relaxed);

    jobject buf = (*env)->NewDirectByteBuffer(env, copy, (jlong)length);
    if (buf == NULL) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ NewDirectByteBuffer returned NULL\n", pid, tid);
        free(copy);
        return NULL;
    }

    return buf;
}

// 종료
JNIEXPORT void JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_closeSharedMemory(
    JNIEnv *env, jobject obj)
{
    if (g_pool_bigbuf_global) {
        (*env)->DeleteGlobalRef(env, g_pool_bigbuf_global);
        g_pool_bigbuf_global = NULL;
    }
    // 필요 시 cleanup_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem");
}
