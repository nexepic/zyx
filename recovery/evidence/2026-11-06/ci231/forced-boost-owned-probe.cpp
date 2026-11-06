#include "graph/utils/OwnedLazyUnorderedMap.hpp"
#include <cassert>
#include "graph/utils/OwnedStdString.hpp"
#include <cstdio>
#include <string>
#include <string_view>
struct Resource final : graph::utils::UnmanagedAllocationResource {
 size_t calls=0, fail=~size_t{0}, live=0;
 void reserveUnmanaged(size_t n)override {if(calls++==fail)throw std::bad_alloc();live+=n;}
 void releaseUnmanaged(size_t n)noexcept override {live-=n;}
 void* do_allocate(size_t n,size_t a) override {if(calls++==fail)throw std::bad_alloc();auto p=std::pmr::new_delete_resource()->allocate(n,a);live+=n;return p;}
 void do_deallocate(void*p,size_t n,size_t a) override {std::pmr::new_delete_resource()->deallocate(p,n,a);live-=n;}
 bool do_is_equal(const memory_resource& x)const noexcept override{return this==&x;}
};
struct Hash {using is_transparent=void;static size_t next(){static size_t n=0;return ++n;} size_t seed=next(); size_t operator()(std::string_view s)const noexcept{return std::hash<std::string_view>{}(s)^seed;}};
struct Equal {using is_transparent=void;bool operator()(std::string_view a,std::string_view b)const noexcept{return a==b;}};
using Map=graph::utils::OwnedLazyUnorderedMap<graph::utils::OwnedStdString,int,Hash,Equal>;
int main(){
 auto observed=std::make_shared<Resource>();
 {graph::utils::OwnedAllocationScope scope(observed);Map m;m.emplace("a-long-owned-projection-name-exercising-nested-allocator-ownership",7);assert(m.find(std::string_view("a-long-owned-projection-name-exercising-nested-allocator-ownership"))->second==7);const auto& c=m;assert(c.find(std::string_view("a-long-owned-projection-name-exercising-nested-allocator-ownership"))->second==7);}
 assert(observed->live==0);const auto count=observed->calls;
 for(size_t i=0;i<count;++i){auto r=std::make_shared<Resource>();r->fail=i;graph::utils::OwnedAllocationScope scope(r);Map m;bool threw=false;try{m.emplace("a-long-owned-projection-name-exercising-nested-allocator-ownership",7);}catch(const std::bad_alloc&){threw=true;}assert(threw&&m.empty()&&r->live==0);r->fail=~size_t{0};m.emplace("a-long-owned-projection-name-exercising-nested-allocator-ownership",7);assert(m.find(std::string_view("a-long-owned-projection-name-exercising-nested-allocator-ownership"))->second==7);m.clear();assert(r->live==0);}
 std::printf("allocation_points=%zu; all failure/refund/retry and heterogeneous lookup checks passed\n",count);
}
