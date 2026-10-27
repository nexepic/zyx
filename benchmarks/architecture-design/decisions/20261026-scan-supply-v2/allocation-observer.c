#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <execinfo.h>
#include <dlfcn.h>
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t,size_t);
extern void *__libc_realloc(void *,size_t);
extern void __libc_free(void *);
static _Atomic int active;
static _Atomic unsigned long allocations,bytes,frees,failed;
static _Atomic unsigned long bins[9];
static __thread int nested;
static unsigned window;
struct Sample {void *frames[20];int count;size_t bytes;};
static struct Sample samples[4096];
static void record(void *pointer,size_t size) {
 if(nested || !atomic_load_explicit(&active,memory_order_relaxed))return;
 nested=1;int saved=errno;
 if(!pointer && size){atomic_fetch_add(&failed,1);nested=0;errno=saved;return;}
 unsigned long index=atomic_fetch_add(&allocations,1);atomic_fetch_add(&bytes,size);
 unsigned bin=0;size_t cap=32;while(bin<8 && size>cap){++bin;cap=bin<6?cap*2:bin==6?4096:bin==7?16384:SIZE_MAX;}atomic_fetch_add(&bins[bin],1);
 if(index%256==0 && index/256<4096){struct Sample *s=&samples[index/256];s->bytes=size;s->count=backtrace(s->frames,20);}
 nested=0;errno=saved;
}
void *malloc(size_t size){void *p=__libc_malloc(size);record(p,size);return p;}
void *calloc(size_t count,size_t size){void *p=__libc_calloc(count,size);record(p,count*size);return p;}
void *realloc(void *old,size_t size){void *p=__libc_realloc(old,size);record(p,size);return p;}
void free(void *p){if(p && !nested && atomic_load_explicit(&active,memory_order_relaxed))atomic_fetch_add(&frees,1);__libc_free(p);}
void zyx_allocation_begin(void) {
 atomic_store(&active,0);nested=1;void *warm[20];backtrace(warm,20);nested=0;
 atomic_store(&allocations,0);atomic_store(&bytes,0);atomic_store(&frees,0);atomic_store(&failed,0);
 for(int i=0;i<9;++i)atomic_store(&bins[i],0);
 atomic_store(&active,1);
}
void zyx_allocation_end(void) {
 atomic_store(&active,0);const char *path=getenv("ZYX_ALLOCATION_OUTPUT");if(!path)return;
 FILE *out=fopen(path,"a");if(!out)return;
 unsigned long count=atomic_load(&allocations);
 fprintf(out,"# window %u allocations %lu requested_bytes %lu frees %lu failed %lu sample_interval 256\n",window++,count,atomic_load(&bytes),atomic_load(&frees),atomic_load(&failed));
 fprintf(out,"# size_bins");for(int i=0;i<9;++i)fprintf(out," %lu",atomic_load(&bins[i]));fprintf(out,"\n");
 unsigned long n=(count+255)/256;if(n>4096)n=4096;
 for(unsigned long i=0;i<n;++i)for(int frame=0;frame<samples[i].count;++frame){
  Dl_info info={0};uintptr_t pc=(uintptr_t)samples[i].frames[frame];
  if(dladdr((void *)pc,&info) && info.dli_fname && info.dli_fbase)fprintf(out,"%lu\t%zu\t%d\t%s\t%lx\n",i,samples[i].bytes,frame,info.dli_fname,(unsigned long)(pc-(uintptr_t)info.dli_fbase));
 }
 fclose(out);
}

extern void *__libc_memalign(size_t,size_t);
int posix_memalign(void **result,size_t alignment,size_t size) {
 if(alignment<sizeof(void *) || (alignment & (alignment-1)))return EINVAL;
 int saved=errno;void *p=__libc_memalign(alignment,size);int status=(!p && size)?ENOMEM:0;
 errno=saved;record(p,size);if(!status)*result=p;return status;
}
void *aligned_alloc(size_t alignment,size_t size) {
 if(!alignment || (alignment & (alignment-1))){errno=EINVAL;return NULL;}
 void *p=__libc_memalign(alignment,size);record(p,size);return p;
}
void *memalign(size_t alignment,size_t size){void *p=__libc_memalign(alignment,size);record(p,size);return p;}
