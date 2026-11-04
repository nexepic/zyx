#include <atomic>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <dlfcn.h>
namespace sample_alloc {
struct Cell { void *ptr; size_t bytes; void *caller; }; static Cell cells[65536];
static std::atomic_flag lock = ATOMIC_FLAG_INIT; static bool active=false; static size_t current=0, peak=0, missing=0; static size_t peak_sizes[256], peak_count=0; static void *peak_callers[256];
void acquire() { while (lock.test_and_set(std::memory_order_acquire)) {} }
void release() { lock.clear(std::memory_order_release); }
void add(void *p,size_t n,void *caller) { acquire(); if(active) { size_t i=0; while(i<65536 && cells[i].ptr) ++i; if(i==65536) ++missing; else { cells[i]={p,n,caller}; current+=n; if(current>peak) { peak=current; peak_count=0; for(const auto &c:cells) if(c.ptr && peak_count<256) {peak_sizes[peak_count]=c.bytes; peak_callers[peak_count++]=c.caller;} } } } release(); }
void del(void *p) { acquire(); if(active) for(size_t i=0;i<65536;++i) if(cells[i].ptr==p) {current-=cells[i].bytes;cells[i]={};break;} release(); }
void start() { acquire(); for(auto &c:cells)c={}; current=peak=missing=0; active=true;release(); }
void stop() { acquire();active=false;release();std::fprintf(stderr,"ALLOC_SAMPLE peak=%zu live=%zu overflow=%zu allocations=%zu\n",peak,current,missing,peak_count); Dl_info info{}; dladdr(reinterpret_cast<void *>(&stop), &info); const auto base=reinterpret_cast<size_t>(info.dli_fbase); for (const auto &c : cells) if (c.ptr) std::fprintf(stderr,"LIVE_SAMPLE_SIZE %zu offset=%zx\n", c.bytes, reinterpret_cast<size_t>(c.caller)-base); for(size_t i=0;i<peak_count;++i) std::fprintf(stderr,"ALLOC_SAMPLE_SIZE %zu offset=%zx\n",peak_sizes[i],reinterpret_cast<size_t>(peak_callers[i])-base); }
}
void *operator new(size_t n) { if(void *p=std::malloc(n?n:1)) {sample_alloc::add(p,n,__builtin_return_address(0));return p;} throw std::bad_alloc(); }
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { if(p){sample_alloc::del(p);std::free(p);} }
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,size_t) noexcept { ::operator delete(p); }
void *operator new(size_t n,std::align_val_t a) { void *p=nullptr; if(posix_memalign(&p,static_cast<size_t>(a),n?n:1)) throw std::bad_alloc();sample_alloc::add(p,n,__builtin_return_address(0));return p; }
void *operator new[](size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void *p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void *p,size_t,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void *p,size_t,std::align_val_t) noexcept { ::operator delete(p); }
