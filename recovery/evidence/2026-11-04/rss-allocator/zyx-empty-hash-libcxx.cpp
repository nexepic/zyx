#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <new>
static bool measuring=false;static size_t calls=0,bytes=0;
void *operator new(size_t n){void*p=std::malloc(n?n:1);if(!p)throw std::bad_alloc();if(measuring){++calls;bytes+=n;}return p;}
void operator delete(void*p)noexcept{std::free(p);}void operator delete(void*p,size_t)noexcept{std::free(p);}
int main(){measuring=true;for(size_t i=0;i<100000;++i){std::unordered_map<int,int> maps[5];for(auto&m:maps)m.reserve(0);}measuring=false;std::printf("EMPTY_HASH libcxx=%d iterations=100000 new_calls=%zu new_bytes=%zu\n",_LIBCPP_VERSION,calls,bytes);}
