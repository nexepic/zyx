#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/utils/StringBufferBytes.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
struct Allocation { void *pointer=nullptr; size_t bytes=0; };
static Allocation allocations[256];
static bool tracking=false;
static size_t live=0, peakRequests=0, requestCount=0;
void *operator new(size_t bytes) {
 void *p=std::malloc(bytes?bytes:1); if(!p) throw std::bad_alloc();
 if(tracking) { bool found=false; for(auto &a:allocations) if(!a.pointer) {
  a={p,bytes}; found=true; break; }
  if(!found) std::abort();
  ++requestCount; live+=bytes; peakRequests=(std::max)(peakRequests,live);
 }
 return p;
}
void operator delete(void *p) noexcept {
 for(auto &a:allocations) if(a.pointer && a.pointer==p) {live-=a.bytes;a={};break;}
 std::free(p);
}
void operator delete(void *p,size_t) noexcept {operator delete(p);}
using namespace graph;
int main() {
 std::vector<std::byte> encoded;
 storage::CanonicalPropertyValueCodec::append(encoded, PropertyValue(std::string(8192,'s')));
 size_t offset=0;
 const auto upper=storage::CanonicalPropertyValueCodec::inspectNext(encoded,offset).maximum_heap_request_bytes;
 auto denied=std::make_shared<query::execution::MaterializedByteBudget>(upper-1);
 {
  storage::detail::PropertyDecodeWorkspace scope(denied); offset=0;
  try { (void)storage::CanonicalPropertyValueCodec::decodeNext(encoded,offset); return 4; }
  catch(const query::execution::MaterializedByteLimitExceeded&) { assert(offset==0 && denied->used()==0); }
 }
 auto memory=std::make_shared<query::execution::MaterializedByteBudget>(1024*1024);
 size_t count=0, retained=0, peak=0; bool failed=false;
 {
  storage::detail::PropertyDecodeWorkspace scope(memory);
  std::array<std::optional<PropertyValue>,64> values;
  tracking=true;
  try {
   for(auto &v:values) {
    offset=0; v.emplace(storage::CanonicalPropertyValueCodec::decodeNext(encoded,offset));
    const auto &s=std::get<std::string>(v->getVariant()); assert(s.size()==8192 && s.front()=='s');
    retained+=utils::detail::dynamicStringBufferBytes(s); ++count;
    peak=(std::max)(peak,memory->used()); assert(memory->used()>=retained);
   }
  } catch(const query::execution::MaterializedByteLimitExceeded&) {failed=true;}
  tracking=false;
  std::printf("new_requests=%zu live_new_bytes=%zu peak_new_bytes=%zu\n",requestCount,live,peakRequests);
  std::printf("libcpp=%d constructed=%zu upper=%zu retained_capacity_bytes=%zu held_bytes=%zu max_observed_held=%zu refused=%d\n",_LIBCPP_VERSION,count,upper,retained,memory->used(),peak,int(failed));
 }
 assert(memory->used()==0 && live==0);
 return failed ? 1 : 0;
}
