#include <typeinfo>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
extern "C" [[noreturn]] void __cxa_throw(void *value, void *type, void (*destroy)(void *)) {
    const auto *info = static_cast<std::type_info *>(type);
    if (std::strstr(info->name(), "ResourceAdmissionDenied")) {
        static constexpr char message[] = "QUOTA_THROW_STACK\n";
        ::write(2, message, sizeof(message)-1U);
        void *frames[48]; const int count = ::backtrace(frames,48); ::backtrace_symbols_fd(frames,count,2);
    }
    using Throw = void (*)(void *, void *, void (*)(void *));
    static auto original = reinterpret_cast<Throw>(::dlsym(RTLD_NEXT,"__cxa_throw"));
    original(value,type,destroy); __builtin_unreachable();
}
