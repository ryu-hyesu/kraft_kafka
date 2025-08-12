#ifndef SHARED_MEMORY_POOL_H
#define SHARED_MEMORY_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdalign.h>
#include "shared_memory.h"

#define POOL_COUNT 4096
#define SAMPLE_SIZE BUF_SIZE

#define SHM_MAGIC 0x53485031u
#define SHM_VER 1u

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t flags;
    uint32_t pool_count;
    uint32_t sample_size;
    uint64_t region_size;

    _Atomic uint32_t init_state;
    _Atomic uint32_t init_epoch;

    alignas(64) _Atomic uint64_t head;
    char pad1[64 - sizeof(_Atomic uint64_t)];
    alignas(64) _Atomic uint64_t tail;
    char pad2[64 - sizeof(_Atomic uint64_t)];

    alignas(64) uint32_t slots[POOL_COUNT];
} shm_pool_meta_t;

typedef struct{
    shm_pool_meta_t meta;
    alignas(64) unsigned char data[POOL_COUNT][SAMPLE_SIZE];
} shm_pool_region_t;

extern shm_pool_region_t *g_pool;

// lock-free freelist ringbuffer 구조체
typedef struct {
    alignas(64) _Atomic uint32_t head;
    char _pad1[64 - sizeof(_Atomic uint32_t)];

    alignas(64) _Atomic uint32_t tail;
    char _pad2[64 - sizeof(_Atomic uint32_t)];

    alignas(64) uint32_t slots[POOL_COUNT];
} shm_memory_pool;


int init_shared_memory_pool();  
unsigned char* shm_pool_get();         // 메모리 할당 (freelist pop)
void shm_pool_release(unsigned char*); // 메모리 반납 (freelist push)

#endif
