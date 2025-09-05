// tls_profiler.c

#include <stdatomic.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>

static atomic_uint g_thread_id_counter = 0;
__thread uint32_t thread_id;
__thread uint64_t t_bk_relax_count = 0;
__thread int tls_initialized = 0;

void init_thread_id(void) {
    if (!tls_initialized) {
        thread_id = atomic_fetch_add(&g_thread_id_counter, 1);
        tls_initialized = 1;
    }
}

void increment_relax_count(void) {
    init_thread_id();
    t_bk_relax_count++;
}

void print_relax_count(void) {
    fprintf(stderr, "[Thread %u] cpu_relax() calls: %" PRIu64 " ",
            thread_id, t_bk_relax_count);
}

