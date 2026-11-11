#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unwind.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t,size_t);
extern void *__libc_realloc(void *,size_t);
extern void __libc_free(void *);
enum { FRAMES=16, SLOTS=2048 };
struct Sample { uintptr_t frames[FRAMES]; unsigned depth; uint64_t hits, bytes; };
static struct Sample samples[SLOTS];
static _Atomic unsigned active;
static _Atomic uint64_t calls, requested, collisions;
static __thread int inside;
static atomic_flag table_lock=ATOMIC_FLAG_INIT;
struct Trace { uintptr_t pc[FRAMES]; unsigned n; };
static _Unwind_Reason_Code frame(struct _Unwind_Context *c,void *p) {
 struct Trace *t=p;
 if(t->n==FRAMES)return _URC_END_OF_STACK;
 t->pc[t->n++]=(uintptr_t)_Unwind_GetIP(c);return _URC_NO_REASON;
}
static void observe(size_t bytes) {
 if(!atomic_load_explicit(&active,memory_order_relaxed)||inside)return;
 inside=1;
 uint64_t n=atomic_fetch_add(&calls,1);atomic_fetch_add(&requested,bytes);
 if(n%128==0) {
  struct Trace t={{0},0};_Unwind_Backtrace(frame,&t);
  uint64_t hash=0;
  for(unsigned i=2;i<t.n&&i<8;i++)hash=hash*131+(t.pc[i]>>3);
  while(atomic_flag_test_and_set(&table_lock)){}
  unsigned found=0;
  for(unsigned j=0;j<SLOTS;j++) {
   struct Sample *s=&samples[(hash+j)%SLOTS];
   if(!s->hits){memcpy(s->frames,t.pc,sizeof(t.pc));s->depth=t.n;s->hits=1;s->bytes=bytes;found=1;break;}
   if(!memcmp(s->frames+2,t.pc+2,6U*sizeof(uintptr_t))){s->hits++;s->bytes+=bytes;found=1;break;}
  }
  if(!found)atomic_fetch_add(&collisions,1);
  atomic_flag_clear(&table_lock);
 }
 inside=0;
}
void *malloc(size_t n){void *p=__libc_malloc(n);if(p)observe(n);return p;}
void *calloc(size_t n,size_t w){void *p=__libc_calloc(n,w);if(p&&(!w||n<=SIZE_MAX/w))observe(n*w);return p;}
void *realloc(void *old,size_t n){void *p=__libc_realloc(old,n);if(p)observe(n);return p;}
void free(void *p){__libc_free(p);}
void ci236_profile_start(void) {
 // Warm the diagnostic unwinder and fixed storage before the query baseline.
 inside=1;struct Trace t={{0},0};_Unwind_Backtrace(frame,&t);inside=0;
 memset(samples,0,sizeof(samples));atomic_store(&calls,0);atomic_store(&requested,0);atomic_store(&collisions,0);atomic_store(&active,1);
}
void ci236_profile_stop(void) {
 atomic_store(&active,0);
 while(atomic_flag_test_and_set(&table_lock)){}
 fprintf(stderr,"CI_QUERY_ALLOC calls=%llu requested=%llu sampling_interval=128 sample_table_overflow=%llu\n",
  (unsigned long long)atomic_load(&calls),(unsigned long long)atomic_load(&requested),(unsigned long long)atomic_load(&collisions));
 for(unsigned j=0;j<32;j++) {
  struct Sample *best=0;
  for(unsigned i=0;i<SLOTS;i++)if(samples[i].hits&&(!best||samples[i].hits>best->hits))best=&samples[i];
  if(!best)break;
  fprintf(stderr,"CI_QUERY_SITE hits=%llu sampled_requested=%llu",(unsigned long long)best->hits,(unsigned long long)best->bytes);
  for(unsigned f=2;f<best->depth;f++) {
   Dl_info d={0};if(dladdr((void *)best->frames[f],&d)&&d.dli_fname) {
    const char *name=strrchr(d.dli_fname,'/');name=name?name+1:d.dli_fname;
    fprintf(stderr," %s+0x%llx",name,(unsigned long long)(best->frames[f]-(uintptr_t)d.dli_fbase));
   }
  }
  fputc('\n',stderr);best->hits=0;
 }
 atomic_flag_clear(&table_lock);
}
