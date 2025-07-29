#ifndef SHARED_MEMORY_POOL_H
#define SHARED_MEMORY_POOL_H

#include <stdint.h>
#include <stddef.h>    // ✅ ptrdiff_t 필요
#include <stdatomic.h>
#include "shared_memory.h"

#define POOL_COUNT 128
#define SAMPLE_SIZE BUF_SIZE
extern unsigned char (*shm_pool)[SAMPLE_SIZE];

int init_shared_memory_pool();  
unsigned char* shm_pool_get();
void shm_pool_release(unsigned char* ptr);

#endif
