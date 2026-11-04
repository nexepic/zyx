#include "graph/query/execution/GuardedRowCopy.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
static bool measuring=false;static size_t calls=0,bytes=0;
void *operator new(size_t n){void*p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();if(measuring){++calls;bytes+=n;}return p;}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,size_t)noexcept{std::free(p);}
void *operator new[](size_t n){return ::operator new(n);}
void operator delete[](void*p)noexcept{std::free(p);}
void operator delete[](void*p,size_t)noexcept{std::free(p);}
int main(){
 graph::query::execution::LogicalPlanReadRow source;
 measuring=true;
 for(size_t i=0;i<100000;++i){auto copy=graph::query::execution::detail::copyLogicalReadRowGuarded(source,[]{});if(!copy.values.empty())return 2;}
 measuring=false;
 std::printf("EMPTY_ROW_COPY iterations=100000 new_calls=%zu new_bytes=%zu\n",calls,bytes);
}
