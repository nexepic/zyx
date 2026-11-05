#include "graph/query/QueryGuard.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
static bool measure=false;static size_t calls=0,bytes=0;
void *operator new(size_t n){void*p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();if(measure){++calls;bytes+=n;}return p;}
void operator delete(void*p)noexcept{std::free(p);}void operator delete(void*p,size_t)noexcept{std::free(p);}
void *operator new[](size_t n){return ::operator new(n);}void operator delete[](void*p)noexcept{std::free(p);}
int main(){if(!graph::query::QueryGuard::getCurrentRSSBytes())return 2;measure=true;size_t last=0;for(size_t i=0;i<1000;++i){last=graph::query::QueryGuard::getCurrentRSSBytes();if(!last)return 3;}measure=false;std::printf("RSS_SAMPLER samples=1000 new_calls=%zu requested_bytes=%zu last_rss=%zu\n",calls,bytes,last);}
