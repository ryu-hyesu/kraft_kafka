// tls_profiler.h

#ifndef TLS_PROFILER_H
#define TLS_PROFILER_H

#pragma once
#include <stdint.h>

extern __thread uint32_t thread_id;
extern __thread uint64_t t_bk_relax_count;
extern __thread int tls_initialized;

void init_thread_id(void);
void increment_relax_count(void);
void print_relax_count(void);

#endif


