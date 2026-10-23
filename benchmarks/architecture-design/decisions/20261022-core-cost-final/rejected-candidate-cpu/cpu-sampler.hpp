// Linux-only diagnostic observer, outside the portable production engine.
#include <signal.h>
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
namespace cpu_sample {
inline std::array<uintptr_t, 65536> pcs{};
inline volatile sig_atomic_t count = 0;
inline std::array<uint32_t, 65536> weights{};
inline timer_t timer{};
inline bool initialized = false, enabled = false;
inline rusage before{};
inline timespec wall_before{};
inline double wall = 0;
inline double user = 0, system = 0;
inline double seconds(timeval t) { return t.tv_sec + t.tv_usec / 1000000.0; }
inline void handler(int, siginfo_t *signal, void *context) {
    const auto i = count;
    if (i < static_cast<sig_atomic_t>(pcs.size())) {
        pcs[static_cast<size_t>(i)] = static_cast<uintptr_t>(static_cast<ucontext_t *>(context)->uc_mcontext.gregs[REG_RIP]);
        weights[static_cast<size_t>(i)] = 1U + static_cast<uint32_t>(signal->si_overrun);
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
            std::fprintf(out,"%s\t%llx\t%u\n",info.dli_fname,static_cast<unsigned long long>(pc-reinterpret_cast<uintptr_t>(info.dli_fbase)),weights[static_cast<size_t>(i)]);
        else std::fprintf(out,"unknown\t%llx\t%u\n",static_cast<unsigned long long>(pc),weights[static_cast<size_t>(i)]);
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
    if (enabled) { itimerspec interval{}; interval.it_value.tv_nsec=1000000; interval.it_interval=interval.it_value;
        if (timer_settime(timer,0,&interval,nullptr)) throw std::runtime_error("sampler timer_settime failed"); }
}
inline void end() {
    if (enabled) { itimerspec stop{}; timer_settime(timer,0,&stop,nullptr); }
    timespec wall_after{}; clock_gettime(CLOCK_MONOTONIC,&wall_after);
    wall += wall_after.tv_sec-wall_before.tv_sec+(wall_after.tv_nsec-wall_before.tv_nsec)/1000000000.0;
    rusage after{}; getrusage(RUSAGE_SELF,&after);
    user += seconds(after.ru_utime)-seconds(before.ru_utime);
    system += seconds(after.ru_stime)-seconds(before.ru_stime);
}
}
