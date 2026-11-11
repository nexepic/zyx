#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <limits.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t,size_t);
extern void *__libc_realloc(void *,size_t);
extern void __libc_free(void *);
static _Atomic uint64_t calls, requested, frees;
static _Atomic int stopped, overflow;
static void add(size_t n) {
 if(atomic_load_explicit(&stopped,memory_order_relaxed))return;
 atomic_fetch_add_explicit(&calls,1,memory_order_relaxed);
 uint64_t before=atomic_fetch_add_explicit(&requested,n,memory_order_relaxed);
 if(before>UINT64_MAX-n)atomic_store(&overflow,1);
}
void *malloc(size_t n){void *p=__libc_malloc(n);if(p)add(n);return p;}
void *calloc(size_t n,size_t w){void *p=__libc_calloc(n,w);if(p){if(w&&n>SIZE_MAX/w)atomic_store(&overflow,1);else add(n*w);}return p;}
void *realloc(void *old,size_t n){void *p=__libc_realloc(old,n);if(p)add(n);return p;}
void free(void *p){if(p&&!atomic_load_explicit(&stopped,memory_order_relaxed))atomic_fetch_add_explicit(&frees,1,memory_order_relaxed);__libc_free(p);}
__attribute__((destructor)) static void report(void){atomic_store(&stopped,1);fprintf(stderr,"CI_PROCESS_ALLOC calls=%llu requested_bytes=%llu free_calls=%llu overflow=%d\n",(unsigned long long)atomic_load(&calls),(unsigned long long)atomic_load(&requested),(unsigned long long)atomic_load(&frees),atomic_load(&overflow));}
