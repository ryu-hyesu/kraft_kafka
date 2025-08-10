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
#include "org_apache_kafka_clients_producer_SharedMemoryProducer.h"

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
    // fprintf(stderr, "[JNI][PRODUCER] init_shared_memory_pool\n");
    if (!g_pool) {
        if (init_shared_memory_pool() != 0) return NULL;
    }

    // fprintf(stderr, "[JNI][PRODUCER] shm_pool_get\n");
    unsigned char* ptr = shm_pool_get();
    if (!ptr) {
        fprintf(stderr, "❌ shm_pool_get() failed: no available slot.\n");
        return NULL;
    }

    // 메모리 접근 가능성 사전 확인(매핑 문제 조기검출)
    volatile unsigned char touch0 = ptr[0];
    volatile unsigned char touchN = ptr[SAMPLE_SIZE - 1];
    (void)touch0; (void)touchN;

    // 혹시 기존 예외가 남아 있으면 클리어
    (*env)->ExceptionClear(env);

    // fprintf(stderr, "[JNI][PRODUCER] create a DirectByteBuffer ptr=%p size=%d\n",
    //         (void*)ptr, SAMPLE_SIZE);

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
        return NULL;
    }

    // fprintf(stderr, "[JNI][PRODUCER] before return a DirectByteBuffer buffer=%p\n",
    //         (void*)buffer);
    return buffer;
}


// 포인터를 받아 ringbuffer에 저장
JNIEXPORT void JNICALL Java_org_apache_kafka_clients_producer_SharedMemoryProducer_commitSharedMemoryByBuffer(JNIEnv *env, jobject obj, jobject buffer, jint length) {
    // fprintf(stderr, "[JNI][COMMIT] init_shared_memory_pool buffer=%p\n",
            // (void*)buffer);
    if (g_pool == NULL) {
        if (init_shared_memory_pool() != 0)
            return;
    }
    
    if (!handle.rb || !handle.semaphore) {
        if (initialize_shared_memory(&handle, "/prod_broker_shm", "/prod_broker_sem", true) != 0) return;
    }

    void *nativeBuffer = (*env)->GetDirectBufferAddress(env, buffer);
    if (!nativeBuffer) return;

    // fprintf(stderr, "[JNI][COMMIT] GetDirectBufferAddress buffer=%p\n",
    //         (void*)nativeBuffer);

    // fprintf(stderr, "start to write\n");
    if (buffer_try_enqueue(handle.rb, (const char *)nativeBuffer, length)) {
        atomic_fetch_add(&enq_success_count, 1);
        sem_post(handle.semaphore);
    } else {
        fprintf(stderr, "fail to msg\n");
    }
}


JNIEXPORT jobject JNICALL
Java_org_apache_kafka_clients_producer_SharedMemoryProducer_readSharedMemoryByBuffer(
    JNIEnv *env, jobject obj) 
{
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
            // 큐 비어있음
            // fprintf(stderr, "[JNI][%d:%lu] (empty)\n", pid, tid);
        }
        return NULL;
    }

    const char *ptr = NULL;
    int length = 0;

    // fprintf(stderr, "start to read");
    if (!buffer_try_dequeue(handle.rb, &ptr, &length)) {
        // 세마포어는 이미 내려갔으므로(sem_trywait 성공) 실패 시 복구할지 정책 결정 필요
        // 생산자/소비자 동기화 정책에 따라 sem_post를 되돌릴지 판단
        fprintf(stderr, "[JNI][%d:%lu] ❌ dequeue failed after sem_trywait; length=%d ptr=%p\n", pid, tid, length, (void*)ptr);
        return NULL;
    }

    // base/offset 계산 및 검사 로그
    unsigned char *base = (unsigned char*)ptr - 4;
    uintptr_t pool_lo = (uintptr_t)&g_pool->data[0][0];
    uintptr_t pool_hi = (uintptr_t)&g_pool->data[POOL_COUNT][0];
    ptrdiff_t off = (unsigned char*)base - &g_pool->data[0][0];
    uint32_t idx = (uint32_t)(off / SAMPLE_SIZE);

    // fprintf(stderr,
    //     "[JNI][%d:%lu] DEQ ok: payload=%p len=%d base=%p idx=%u off=%td pool=[%p..%p)\n",
    //     pid, tid, (void*)ptr, length, (void*)base, idx, off, (void*)pool_lo, (void*)pool_hi);

    // ---- 안전한 방법 (B): 별도 메모리에 복사 후 풀 즉시 반납 ----
    void *copy = malloc((size_t)length);
    if (!copy) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ malloc(%d) failed\n", pid, tid, length);
        // 풀 반납은 해야 함
        shm_pool_release(base);
        return NULL;
    }
    memcpy(copy, ptr, (size_t)length);

    // 이제 풀 반납 (복사 끝났으므로 안전)
    shm_pool_release(base);
    // 카운터
    atomic_fetch_add_explicit(&deq_success_count, 1, memory_order_relaxed);

    // 디버그 카운터 출력(선택)
    uint64_t cnt = (uint64_t)atomic_load(&deq_success_count);
    // fprintf(stderr, "[JNI][%d:%lu] 🎯 deq_success_count=%" PRIu64 "\n", pid, tid, cnt);

    // DirectByteBuffer로 자바에 전달 (free는 자바에서 별도 JNI로 받거나, 아래처럼 Cleaner 등록을 고려)
    jobject buf = (*env)->NewDirectByteBuffer(env, copy, (jlong)length);
    if (buf == NULL) {
        fprintf(stderr, "[JNI][%d:%lu] ❌ NewDirectByteBuffer returned NULL\n", pid, tid);
        free(copy);
        return NULL;
    }

    return buf;
}
