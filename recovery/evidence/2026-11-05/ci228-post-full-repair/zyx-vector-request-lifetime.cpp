#include <cassert>
#include <cstdio>
#include <optional>
#include "graph/utils/OwnedStdString.hpp"
#include "graph/utils/OwnedStdVector.hpp"
struct Account final : graph::utils::UnmanagedAllocationResource {
 size_t used=0;
 void reserveUnmanaged(size_t n) override {used+=n;}
 void releaseUnmanaged(size_t n) noexcept override {assert(n<=used);used-=n;}
 void *do_allocate(size_t n,size_t a) override {return std::pmr::new_delete_resource()->allocate(n,a);}
 void do_deallocate(void*p,size_t n,size_t a) override {std::pmr::new_delete_resource()->deallocate(p,n,a);}
 bool do_is_equal(const std::pmr::memory_resource&o)const noexcept override{return this==&o;}
};
struct Request {std::string index;std::vector<float> vector;};
int main(){
 auto account=std::make_shared<Account>();graph::utils::OwnedAllocationScope scope(account);
 std::optional<graph::utils::OwnedStdString> name(std::in_place,"memory_embedding");
 std::optional<graph::utils::OwnedStdVector<float>> vector;
 graph::utils::OwnedHeapReservation proxies(account,4U*graph::utils::detail::standardContainerProxyBytes());
 std::optional<Request> request(std::in_place);
 request->index=std::move(static_cast<std::string&>(*name));
 vector.emplace();vector->resize(750U,1.0F);request->vector=std::move(static_cast<std::vector<float>&>(*vector));
 assert(request->vector.size()==750U && request->vector.front()==1.0F);
 std::printf("REQUEST_LIFETIME string_size=%zu name_capacity=%zu vector_bytes=%zu charge=%zu\n",sizeof(std::string),request->index.capacity(),request->vector.capacity()*sizeof(float),account->used);
 assert(account->used>=750U*sizeof(float));
 request.reset();vector.reset();name.reset();proxies.set(0U);
 std::printf("REQUEST_LIFETIME after_data_destroy_and_credit_refund=%zu\n",account->used);assert(account->used==0U);
}
