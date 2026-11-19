#pragma once
#include "graph/debug/CiQuotaStack.hpp"
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#endif
namespace graph::test::ci_quota_stack {
inline std::atomic<unsigned> attempts{0}, captured{0};
inline unsigned limit=0;
inline unsigned long ownerPid=0;
inline unsigned long pid() noexcept {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}
inline void capture() noexcept {
    // Forked death-test children must not consume or report the parent's quota.
    if(pid()!=ownerPid)return;
    const auto index=attempts.fetch_add(1,std::memory_order_relaxed);
    if(index>=limit)return;
    void *frames[32]{};
#ifdef _WIN32
    const auto count=CaptureStackBackTrace(0,32,frames,nullptr);
#else
    const auto count=backtrace(frames,32);
#endif
    std::fprintf(stderr,"CI_QUOTA_NATIVE_STACK stack=%u frames=%u limit=%u\n",index,static_cast<unsigned>(count),limit);
    for(unsigned i=0;i<static_cast<unsigned>(count);++i){
        void *base=nullptr;const char *name="unknown";
#ifdef _WIN32
        HMODULE module=nullptr;char moduleName[1024]{};
        if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(frames[i]),&module)){
            base=module;
            const auto size=GetModuleFileNameA(module,moduleName,static_cast<DWORD>(sizeof(moduleName)));
            if(size && size<sizeof(moduleName))name=moduleName;
        }
#else
        Dl_info info{};
        if(dladdr(frames[i],&info)){base=info.dli_fbase;if(info.dli_fname)name=info.dli_fname;}
#endif
        std::fprintf(stderr,"CI_QUOTA_NATIVE_PC stack=%u index=%u pc=%p base=%p offset=0x%llx module=%s\n",
            index,i,frames[i],base,static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])-reinterpret_cast<uintptr_t>(base)),name);
    }
    std::fflush(stderr);
    if(count>0)captured.fetch_add(1,std::memory_order_relaxed);
}
inline bool initialize() noexcept {
    if(!std::getenv("ZYX_CI_QUOTA_NATIVE"))return true;
    if(!std::getenv("ZYX_CI_QUOTA_STATUS_PATH"))return false;
    const char *value=std::getenv("ZYX_CI_QUOTA_STACK_LIMIT");
    if(!value || std::strlen(value)!=1 || *value<'1' || *value>'3')return false;
    limit=static_cast<unsigned>(*value-'0');ownerPid=pid();
#ifndef _WIN32
    // Resolve the unwinder before test allocation probes and budget scopes.
    // glibc may load libgcc lazily on the first backtrace call.
    void *warmup[1]{};
    (void)backtrace(warmup,1);
#endif
    debug::ciQuotaStackCapture=&capture;
    return true;
}
inline bool finish(int exitCode) noexcept {
    if(!limit)return true;
    const char *path=std::getenv("ZYX_CI_QUOTA_STATUS_PATH");
    if(!path)return true;
    if(pid()!=ownerPid)return false;
    auto *file=std::fopen(path,"w");if(!file)return false;
    const int result=std::fprintf(file,"{\"exited\":true,\"exit_code\":%d,\"native\":true,\"attempts\":%u,\"captured\":%u,\"limit\":%u}\n",
        exitCode,attempts.load(),captured.load(),limit);
    const int closed=std::fclose(file);
    return result>0 && closed==0;
}
}
#endif
