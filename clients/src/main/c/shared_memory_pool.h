#ifndef SHARED_MEMORY_POOL_H
#define SHARED_MEMORY_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdalign.h>
#include "shared_memory.h"

#define POOL_COUNT 1024
#define SAMPLE_SIZE BUF_SIZE

// header
// extern unsigned char (*shm_pool)[SAMPLE_SIZE];

typedef struct {
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    alignas(64) uint32_t slots[POOL_COUNT];
    _Atomic uint32_t init_magic;
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
