#define _GNU_SOURCE
#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t,size_t);
extern void *__libc_realloc(void *,size_t);
extern void *__libc_memalign(size_t,size_t);
static atomic_int enabled;
static atomic_flag lock=ATOMIC_FLAG_INIT;
static __thread int nested;
static uintptr_t base, text_first, text_last;
static const char *output;
enum {MPROF_SLOTS=16384, MPROF_FRAMES=8};
struct Entry {uintptr_t pc[MPROF_FRAMES];uint64_t requests,bytes,largest;};
static struct Entry entries[MPROF_SLOTS];
static uint64_t overflow;
static int image(struct dl_phdr_info *info,size_t size,void *unused) {
    (void)size;(void)unused;
    if(info->dlpi_name && info->dlpi_name[0]) return 0;
    base=info->dlpi_addr;
    for(int i=0;i<info->dlpi_phnum;++i)if(info->dlpi_phdr[i].p_type==PT_LOAD && (info->dlpi_phdr[i].p_flags&PF_X)) {
        text_first=base+info->dlpi_phdr[i].p_vaddr;text_last=text_first+info->dlpi_phdr[i].p_memsz;
    }
    return 1;
}
static void start(int value) {(void)value;atomic_store_explicit(&enabled,1,memory_order_relaxed);}
__attribute__((constructor)) static void initialize(void) {
    output=getenv("ZYX_ALLOC_PROFILE_OUT");if(!output)return;
    dl_iterate_phdr(image,NULL);
    struct sigaction action={0};action.sa_handler=start;sigemptyset(&action.sa_mask);sigaction(SIGUSR2,&action,NULL);
}
static void record(size_t bytes) {
    if(nested || !atomic_load_explicit(&enabled,memory_order_relaxed))return;
    nested=1;
    void *stack[48];const int n=backtrace(stack,48);
    uintptr_t key[MPROF_FRAMES]={0};size_t count=0,h=0;
    for(int i=0;i<n && count<MPROF_FRAMES;++i) {
        uintptr_t pc=(uintptr_t)stack[i];if(pc<text_first || pc>=text_last)continue;
        key[count++]=pc-base;h=(h*33U)^(pc>>4U);
    }
    if(!count){nested=0;return;}
    while(atomic_flag_test_and_set_explicit(&lock,memory_order_acquire)){}
    size_t i;
    for(i=0;i<MPROF_SLOTS;++i) {
        struct Entry *e=&entries[(h+i)%MPROF_SLOTS];
        if(e->requests && memcmp(e->pc,key,sizeof key))continue;
        if(!e->requests)memcpy(e->pc,key,sizeof key);
        ++e->requests;e->bytes+=bytes;if(bytes>e->largest)e->largest=bytes;break;
    }
    if(i==MPROF_SLOTS)++overflow;
    atomic_flag_clear_explicit(&lock,memory_order_release);
    nested=0;
}
void *malloc(size_t n){void *p=__libc_malloc(n);if(p)record(n);return p;}
void *calloc(size_t n,size_t size){void *p=__libc_calloc(n,size);if(p && (!size || n<=SIZE_MAX/size))record(n*size);return p;}
void *realloc(void *old,size_t n){void *p=__libc_realloc(old,n);if(p)record(n);return p;}
void *memalign(size_t alignment,size_t n){void *p=__libc_memalign(alignment,n);if(p)record(n);return p;}
void *aligned_alloc(size_t alignment,size_t n){void *p=__libc_memalign(alignment,n);if(p)record(n);return p;}
int posix_memalign(void **out,size_t alignment,size_t n){
    if(alignment<sizeof(void *) || (alignment&(alignment-1U)))return EINVAL;
    const int saved=errno;void *p=__libc_memalign(alignment,n);const int code=p?0:(errno?errno:ENOMEM);errno=saved;
    if(p){*out=p;record(n);}return code;
}
__attribute__((destructor)) static void finish(void) {
    atomic_store_explicit(&enabled,0,memory_order_relaxed);if(!output)return;
    nested=1;FILE *file=fopen(output,"w");if(!file)return;
    fprintf(file,"# main_base=%#lx overflow=%lu cumulative_requests_not_live_bytes\n",(unsigned long)base,(unsigned long)overflow);
    fputs("requests,bytes,largest,p0,p1,p2,p3,p4,p5,p6,p7\n",file);
    for(size_t i=0;i<MPROF_SLOTS;++i){const struct Entry *e=&entries[i];if(!e->requests)continue;
        fprintf(file,"%lu,%lu,%lu",(unsigned long)e->requests,(unsigned long)e->bytes,(unsigned long)e->largest);
        for(size_t j=0;j<MPROF_FRAMES;++j)fprintf(file,",%#lx",(unsigned long)e->pc[j]);fputc('\n',file);
    }
    fclose(file);
}
