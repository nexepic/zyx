#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include <cstdio>
#include <cassert>
using namespace graph;
int main() {
 std::vector<std::byte> encoded;
 storage::CanonicalPropertyValueCodec::append(encoded,PropertyValue(std::string(3000,'s')));
 auto budget=std::make_shared<query::execution::MaterializedByteBudget>(4096U);
 bool denied=false;size_t peak=0,offset=0;
 {
  storage::detail::PropertyDecodeWorkspace scope(budget);
  try {
   const auto value=storage::CanonicalPropertyValueCodec::decodeNext(encoded,offset);
   assert(std::get<std::string>(value.getVariant())==std::string(3000,'s'));
   assert(offset==encoded.size());peak=budget->used();assert(peak>=3001U && peak<=4096U);
  } catch(const query::execution::MaterializedByteLimitExceeded&) {denied=true;assert(offset==0);}
 }
 assert(budget->used()==0);
 std::printf("libcpp=%d budget=4096 length=3000 denied=%d peak=%zu final=%zu\n",_LIBCPP_VERSION,int(denied),peak,budget->used());
 return denied?1:0;
}
