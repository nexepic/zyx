#include <typeinfo>
#include <cstring>
#include <cstdlib>
#include <dlfcn.h>
#include <execinfo.h>
extern "C" [[noreturn]] void __cxa_throw(void *value, std::type_info *type, void (*destroy)(void *)) {
 if (std::strstr(type->name(), "MaterializedByteLimitExceeded")) {
  void *frames[48]; int n=backtrace(frames,48);backtrace_symbols_fd(frames,n,2);
 }
 using Throw=void(*)(void*,std::type_info*,void(*)(void*));
 auto next=reinterpret_cast<Throw>(dlsym(RTLD_NEXT,"__cxa_throw"));next(value,type,destroy);std::abort();
}
