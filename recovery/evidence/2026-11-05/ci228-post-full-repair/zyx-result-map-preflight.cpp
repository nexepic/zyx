#include <cstdio>
#include <memory>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"
struct Requests {inline static size_t live=0;};
template<class T> struct TrackingAllocator {
 using value_type=T;
 TrackingAllocator()=default;template<class U>TrackingAllocator(const TrackingAllocator<U>&){}
 T *allocate(size_t n){auto *p=std::allocator<T>{}.allocate(n);Requests::live+=n*sizeof(T);return p;}
 void deallocate(T*p,size_t n){Requests::live-=n*sizeof(T);std::allocator<T>{}.deallocate(p,n);}
 template<class U>bool operator==(const TrackingAllocator<U>&)const noexcept{return true;}
};
int main(){
 using Pair=std::pair<const std::string,graph::PropertyValue>;
 using Map=std::unordered_map<std::string,graph::PropertyValue,std::hash<std::string>,std::equal_to<std::string>,TrackingAllocator<Pair>>;
 for(size_t count: {0U,1U,8U}){
  {Map map;map.reserve(count);for(size_t i=0;i<count;++i)map.emplace(std::to_string(i),graph::PropertyValue(static_cast<int64_t>(i)));
   // Current inline/complete result preflight: header plus key bytes,
   // embedded PropertyValue and its 8-byte numeric logical payload.
   const size_t predicted=sizeof(graph::PropertyValue::MapType)+count*(1U+sizeof(graph::PropertyValue)+sizeof(int64_t));
   const size_t actual=sizeof(graph::PropertyValue::MapType)+Requests::live;
   std::printf("PUBLIC_MAP_PREFLIGHT items=%zu public_header=%zu tracking_header=%zu heap_requests=%zu logical_preflight=%zu actual_owned=%zu\n",count,sizeof(graph::PropertyValue::MapType),sizeof(Map),Requests::live,predicted,actual);
  }
  if(Requests::live) return 1;
 }
}
