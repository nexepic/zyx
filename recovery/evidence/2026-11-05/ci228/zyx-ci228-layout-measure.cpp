#include "query/execution/BoundColumnLayout.hpp"
#include <cstdio>
#include <cassert>
using namespace graph::query::execution;
struct Requests:std::pmr::memory_resource {
 size_t calls=0,live=0,peak=0;
 void *do_allocate(size_t n,size_t a) override {auto*p=std::pmr::new_delete_resource()->allocate(n,a);++calls;live+=n;peak=std::max(peak,live);return p;}
 void do_deallocate(void*p,size_t n,size_t a) override {live-=n;std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource&o) const noexcept override{return this==&o;}
};
int main(){
 std::vector<std::string> names;for(size_t i=0;i<256;++i)names.push_back("c"+std::to_string(i));
 MaterializedByteBudget budget(8192);Requests resource;bool denied=false;size_t retained=0;
 try{detail::BoundColumnLayout layout(names,budget,&resource);retained=budget.used();assert(retained==resource.live);for(size_t i=0;i<256;++i)assert(layout.name(layout.column(i))==names[i]);}
 catch(const MaterializedByteLimitExceeded&){denied=true;}
 assert(budget.used()==0&&resource.live==0);
 std::printf("layout columns=256 budget=8192 denied=%d allocations=%zu retained=%zu peak=%zu final=%zu\n",int(denied),resource.calls,retained,resource.peak,budget.used());return denied?1:0;
}
