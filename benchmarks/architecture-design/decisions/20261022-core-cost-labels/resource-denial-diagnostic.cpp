#include <typeinfo>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
extern "C" [[noreturn]] void __cxa_throw(void *object, std::type_info *type, void (*destroy)(void *)) {
    using Throw = void (*)(void *, std::type_info *, void (*)(void *));
    static const auto realThrow = reinterpret_cast<Throw>(dlsym(RTLD_NEXT, "__cxa_throw"));
    if (type && std::strstr(type->name(), "ResourceAdmissionDenied")) {
        void *frames[48]; const int count = backtrace(frames, 48);
        const char message[]="ZYX resource-denial stack\n"; write(STDERR_FILENO,message,sizeof(message)-1U);
        backtrace_symbols_fd(frames,count,STDERR_FILENO);
    }
    realThrow(object,type,destroy); __builtin_unreachable();
}
