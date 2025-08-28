// #define BACKOFF_PROF

#include "shared_memory.h"
#include "shared_memory_pool.h"
#include <jni.h>
#include <string.h>
#include <stdlib.h>
// #include <stdlib.h>   // malloc/free
#include <unistd.h>   // getpid
// #include <inttypes.h>
#include <pthread.h>    // pthread_self
#include <errno.h>      // errno, EAGAIN
#include <inttypes.h>   // PRIu64

#include <stdatomic.h>
#include <stdint.h>
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

_Atomic uint64_t g_bk_relax_tot = 0, g_bk_yield_tot = 0, g_bk_nanos_tot = 0;
_Atomic uint64_t g_bk_yield_ns_tot = 0, g_bk_nanos_ns_tot = 0;

_Atomic uint64_t g_enq_total_ns = 0;
_Atomic uint64_t g_enq_cnt = 0;
_Atomic uint64_t g_enq_cap_wait_ns = 0;     // 용량 대기(consumer가 못 따라와서 full일 때)
_Atomic uint64_t g_enq_pub_cas_wait_ns = 0; 

_Atomic uint64_t g_alloc_total_ns = 0;
_Atomic uint64_t g_alloc_cnt = 0;
_Atomic uint64_t g_alloc_cap_wait_ns = 0;     // 용량 대기(consumer가 못 따라와서 full일 때)
_Atomic uint64_t g_alloc_pub_cas_wait_ns = 0; // prod_pub CAS 전후 대기

_Atomic uint64_t g_deq_total_ns = 0;
_Atomic uint64_t g_deq_cnt = 0;
_Atomic uint64_t g_deq_cas_wait_ns = 0; 

static SharedMemoryHandle handle = {0};

__attribute__((constructor))
static void shm_ctor(void) {
    setvbuf(stderr, NULL, _IONBF, 0); // stderr 무버퍼
    fprintf(stderr, "[SHM][ctor] libsharedmemory loaded (pid=%d)\n", getpid());
}
// producer 메모리 할당
JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_allocateSharedMemoryByBuffer
  (JNIEnv *env, jobject obj)
{
    if (!g_pool) {
        if (init_shared_memory_pool() != 0) return NULL;
    }

#ifdef BACKOFF_PROF
    uint64_t t0_alloc = bk_now_ns();
    uint64_t cap_wait_ns = 0, pubcas_wait_ns = 0;  // 여기선 측정 불가 → 0
#endif

    unsigned char* ptr = shm_pool_get();
    if (!ptr) {
        fprintf(stderr, "❌ shm_pool_get() failed: no available slot.\n");
#ifdef BACKOFF_PROF
        bk_alloc_add(bk_now_ns() - t0_alloc, cap_wait_ns, pubcas_wait_ns);  // 실패도 기록
#endif
        return NULL;
    }

    // 메모리 접근 가능성 사전 확인(매핑 문제 조기검출)
    volatile unsigned char touch0 = ptr[0];
    volatile unsigned char touchN = ptr[SAMPLE_SIZE - 1];
    (void)touch0; (void)touchN;

    // 혹시 기존 예외가 남아 있으면 클리어
    (*env)->ExceptionClear(env);

    // ★ 문제 지점
    jobject buffer = (*env)->NewDirectByteBuffer(env, (void*)ptr, (jlong)SAMPLE_SIZE);

    // 즉시 예외 체크(이 지점에서 반환 못하면 VM 내부에서 막힌 것)
    if ((*env)->ExceptionCheck(env)) {
        fprintf(stderr, "❌ Exception in NewDirectByteBuffer (ptr=%p size=%d)\n",
                (void*)ptr, SAMPLE_SIZE);
        (*env)->ExceptionDescribe(env);   // 콘솔에 스택 출력
        (*env)->ExceptionClear(env);
    }

    if (buffer == NULL) {
        fprintf(stderr, "❌ NewDirectByteBuffer returned NULL (ptr=%p size=%d)\n",
                (void*)ptr, SAMPLE_SIZE);
        shm_pool_release(ptr);
#ifdef BACKOFF_PROF
        bk_alloc_add(bk_now_ns() - t0_alloc, cap_wait_ns, pubcas_wait_ns);  // 실패도 기록
#endif
        return NULL;
    }
#ifdef BACKOFF_PROF
    bk_alloc_add(bk_now_ns() - t0_alloc, cap_wait_ns, pubcas_wait_ns);      // 성공 기록
#endif
    return buffer;
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


// 포인터를 받아 ringbuffer에 저장
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

#ifdef BACKOFF_PROF
    uint64_t t0_enq = bk_now_ns();
    uint64_t cap_wait_ns = 0, pubcas_wait_ns = 0;  // 여기선 직접 계측 불가 → 0
#endif

    if (buffer_try_enqueue(handle.rb, (const char *)nativeBuffer, length)) {
        atomic_fetch_add(&enq_success_count, 1);
        sem_post(handle.semaphore);
#ifdef BACKOFF_PROF
        bk_enq_add(bk_now_ns() - t0_enq, cap_wait_ns, pubcas_wait_ns);       // 성공만 기록
#endif
    } else {
        fprintf(stderr, "fail to msg\n");
    }
}


JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByBuffer(
    JNIEnv *env, jobject obj) 
{

#ifdef BACKOFF_PROF
    uint64_t t0 = bk_now_ns();
    uint64_t cas_wait_ns = 0;    // 이 레벨에서는 측정 불가 → 0 유지
#endif
    pid_t pid = getpid();
    unsigned long tid = (unsigned long)pthread_self();

    if (g_pool == NULL) {
        fprintf(stderr, "[JNI][%d:%lu] init_shared_memory_pool()...\n", pid, tid);
        if (init_shared_memory_pool() != 0) {
            fprintf(stderr, "[JNI][%d:%lu] ❌ init_shared_memory_pool failed\n", pid, tid);
            return NULL;
        }
    }

    if (!handle.rb || !handle.semaphore) {
        fprintf(stderr, "[JNI][%d:%lu] initialize_shared_memory()...\n", pid, tid);
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) {
            fprintf(stderr, "[JNI][%d:%lu] ❌ initialize_shared_memory failed\n", pid, tid);
            return NULL;
        }
    }
    
    if (sem_trywait(handle.semaphore) == -1) {
        int e = errno;
        if (e != EAGAIN) {
            fprintf(stderr, "[JNI][%d:%lu] ⚠️ sem_trywait error: %s\n", pid, tid, strerror(e));
        } else {
        }
        return NULL;
    }

    const char *ptr = NULL;
    int length = 0;

    // fprintf(stderr, "start to read");
    if (!buffer_try_dequeue(handle.rb, &ptr, &length)) {
        // 세마포어는 이미 내려갔으므로(sem_trywait 성공) 실패 시 복구할지 정책 결정 필요
        // 생산자/소비자 동기화 정책에 따라 sem_post를 되돌릴지 판단
        // sem_post(handle.semaphore);
        fprintf(stderr, "[JNI][%d:%lu] ❌ dequeue failed after sem_trywait; length=%d ptr=%p\n", pid, tid, length, (void*)ptr);
        return NULL;
    }
#ifdef BACKOFF_PROF
    // ✅ 성공한 dequeue에 대해서만 누적
    bk_deq_add(bk_now_ns() - t0, cas_wait_ns);
#endif
    jobject buf = (*env)->NewDirectByteBuffer(env, (void*)ptr, (jlong)length);
    if (buf == NULL) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ NewDirectByteBuffer returned NULL\n", pid, tid);
        // free(copy);
        return NULL;
    }
#ifdef BACKOFF_PROF
    // ✅ E2E 총 시간: sem_trywait~dequeue~memcpy~release~NewDirectByteBuffer 포함
    bk_deq_add(bk_now_ns() - t0, cas_wait_ns);
#endif
    return buf;
}

JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_releaseSharedmemoryByBuffer(
    JNIEnv *env, jobject obj, jobject buffer)
{
    if (!buffer) return;
    void *addr = (*env)->GetDirectBufferAddress(env, buffer);
    if(!addr) return;
    unsigned char *base = (unsigned char*)addr - 4;
    shm_pool_release(base);
}

JNIEXPORT void JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_closeSharedMemory(
    JNIEnv *env, jobject obj)
{
    #ifdef BACKOFF_PROF
        bk_print_times_ext();  // ✅ 숫자 찍기
    #endif
    // cleanup_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem");
    return;
}