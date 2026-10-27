#include "cpu-phase.hpp"
// Linux-only diagnostic observer, outside the portable production engine.
#include <signal.h>
#include <unwind.h>
#include <time.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
namespace cpu_sample {
inline std::array<uintptr_t, 65536> pcs{};
inline std::array<std::array<uintptr_t,32>,65536> stacks{};
inline std::array<unsigned,65536> ordinals{};
struct StackWriter { uintptr_t *frames; unsigned size=0; };
inline _Unwind_Reason_Code unwindFrame(_Unwind_Context *context, void *argument) {
 auto &writer=*static_cast<StackWriter *>(argument);
 if(writer.size==32)return _URC_END_OF_STACK;
 writer.frames[writer.size++]=_Unwind_GetIP(context); return _URC_NO_REASON;
}
inline volatile sig_atomic_t count = 0;
inline std::array<uint32_t, 65536> weights{};
inline std::array<uint8_t,65536> phases{};
inline timer_t timer{};
inline bool initialized = false, enabled = false;
inline bool collect = false;
inline unsigned calls = 0;
inline double last_user = 0, last_system = 0;
inline long last_involuntary = 0, last_faults = 0;
inline rusage before{};
inline timespec wall_before{};
inline double wall = 0;
inline double user = 0, system = 0;
inline double seconds(timeval t) { return t.tv_sec + t.tv_usec / 1000000.0; }
inline void handler(int, siginfo_t *signal, void *context) {
    const auto i = count;
    if (i < static_cast<sig_atomic_t>(pcs.size())) {
        pcs[static_cast<size_t>(i)] = static_cast<uintptr_t>(static_cast<ucontext_t *>(context)->uc_mcontext.gregs[REG_RIP]);
        StackWriter writer{stacks[static_cast<size_t>(i)].data()};
        _Unwind_Backtrace(unwindFrame,&writer);
        ordinals[static_cast<size_t>(i)]=calls;
        weights[static_cast<size_t>(i)] = 1U + static_cast<uint32_t>(signal->si_overrun);
        phases[static_cast<size_t>(i)] = cpu_phase::current;
        count = i + 1;
    }
}
inline void finish() {
    const char *path = std::getenv("ZYX_CPU_SAMPLE_OUTPUT"); if (!path) return;
    auto *out = std::fopen(path, "w"); if (!out) return;
    std::fprintf(out,"# user_cpu_seconds %.9f system_cpu_seconds %.9f wall_seconds %.9f samples %d\n",user,system,wall,int(count));
    for (sig_atomic_t i=0;i<count;++i) {
        Dl_info info{}; const auto pc = pcs[static_cast<size_t>(i)];
        if (dladdr(reinterpret_cast<void *>(pc), &info) && info.dli_fname && info.dli_fbase)
            std::fprintf(out,"%s\t%llx\t%u\t%u\n",info.dli_fname,static_cast<unsigned long long>(pc-reinterpret_cast<uintptr_t>(info.dli_fbase)),weights[static_cast<size_t>(i)], unsigned(phases[static_cast<size_t>(i)]));
        else std::fprintf(out,"unknown\t%llx\t%u\t%u\n",static_cast<unsigned long long>(pc),weights[static_cast<size_t>(i)], unsigned(phases[static_cast<size_t>(i)]));
    }
    std::fclose(out);
    const std::string stackPath=std::string(path)+".stacks";
    out=std::fopen(stackPath.c_str(),"w"); if(!out)return;
    for(sig_atomic_t i=0;i<count;++i)for(size_t j=0;j<32 && stacks[i][j];++j) {
      Dl_info info{};const auto pc=stacks[i][j];
      if(dladdr(reinterpret_cast<void *>(pc),&info) && info.dli_fname && info.dli_fbase)
        std::fprintf(out,"%d\t%u\t%zu\t%s\t%llx\n",int(i),ordinals[i],j,info.dli_fname,
          static_cast<unsigned long long>(pc-reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    std::fclose(out);
}
inline void begin() {
    if (!initialized) {
        enabled = std::getenv("ZYX_CPU_SAMPLE_ENABLE") != nullptr;
        if (enabled) {
            struct sigaction action{}; action.sa_sigaction=handler; action.sa_flags=SA_SIGINFO|SA_RESTART;
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGRTMIN+3,&action,nullptr)) throw std::runtime_error("sampler sigaction failed");
            sigevent event{}; event.sigev_notify=SIGEV_THREAD_ID; event.sigev_signo=SIGRTMIN+3;
            event._sigev_un._tid=static_cast<pid_t>(syscall(SYS_gettid));
            if (timer_create(CLOCK_THREAD_CPUTIME_ID,&event,&timer)) throw std::runtime_error("sampler timer_create failed");
        }
        std::atexit(finish); initialized=true;
    }
    clock_gettime(CLOCK_MONOTONIC,&wall_before);
    getrusage(RUSAGE_SELF,&before);
    const unsigned ordinal=calls++;
    uint64_t x=uint64_t(ordinal/2U)+0x92873ULL;
    x=(x^(x>>30U))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27U))*0x94d049bb133111ebULL;x^=x>>31U;
    collect = enabled && bool((x&1U)^(ordinal&1U));
    if (collect) { itimerspec interval{}; interval.it_value.tv_nsec=10000000; interval.it_interval=interval.it_value;
        if (timer_settime(timer,0,&interval,nullptr)) throw std::runtime_error("sampler timer_settime failed"); }
}
inline void end() {
    if (collect) { itimerspec stop{}; timer_settime(timer,0,&stop,nullptr); }
    timespec wall_after{}; clock_gettime(CLOCK_MONOTONIC,&wall_after);
    wall += wall_after.tv_sec-wall_before.tv_sec+(wall_after.tv_nsec-wall_before.tv_nsec)/1000000000.0;
    rusage after{}; getrusage(RUSAGE_SELF,&after);
    last_user = seconds(after.ru_utime)-seconds(before.ru_utime);
    last_involuntary = after.ru_nivcsw-before.ru_nivcsw;
    last_faults = after.ru_majflt-before.ru_majflt;
    user += last_user;
    last_system = seconds(after.ru_stime)-seconds(before.ru_stime);
    system += last_system;
}
}
