#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/utils/StringBufferBytes.hpp"
#include <array>
#include <cassert>
#include <cstdio>
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
  try {
   for(auto &v:values) {
    offset=0; v.emplace(storage::CanonicalPropertyValueCodec::decodeNext(encoded,offset));
    const auto &s=std::get<std::string>(v->getVariant()); assert(s.size()==8192 && s.front()=='s');
    retained+=utils::detail::dynamicStringBufferBytes(s); ++count;
    peak=(std::max)(peak,memory->used()); assert(memory->used()>=retained);
   }
  } catch(const query::execution::MaterializedByteLimitExceeded&) {failed=true;}
  std::printf("libcpp=%d constructed=%zu upper=%zu retained_capacity_bytes=%zu held_bytes=%zu max_observed_held=%zu refused=%d\n",_LIBCPP_VERSION,count,upper,retained,memory->used(),peak,int(failed));
 }
 assert(memory->used()==0);
 return failed ? 1 : 0;
}
