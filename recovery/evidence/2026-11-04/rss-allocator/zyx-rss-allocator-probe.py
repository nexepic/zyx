from pathlib import Path
import os,shlex,subprocess
root=Path('/workspace/zyx-ci-fix');build=Path('/workspace/zyx-ci-config');out=Path('/tmp/zyx-rss-allocator-probe');out.mkdir(exist_ok=True)
prefix = r"""
#include "CypherParserImpl.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <charconv>
static size_t residentWithoutAllocation() {
    char data[128];
    const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0U;
    const auto length = ::read(fd, data, sizeof(data)); ::close(fd);
    if (length <= 0) return 0U;
    size_t total = 0U, resident = 0U;
    const auto first = std::from_chars(data, data + length, total);
    if (first.ec != std::errc{}) return 0U;
    auto start = first.ptr; while (start < data + length && *start == ' ') ++start;
    const auto second = std::from_chars(start, data + length, resident);
    const auto page = ::sysconf(_SC_PAGESIZE);
    return second.ec == std::errc{} && page > 0 ? resident * static_cast<size_t>(page) : 0U;
}
"""
allocationHooks = '#include <atomic>\n#include <cstdio>\n#include <dlfcn.h>\n#include <cstdint>\n#include <array>\n#include <algorithm>\nextern "C" size_t __asan_get_alloc_stack(void *, void **, size_t, int *);\nnamespace rss_probe {\nstatic std::atomic<bool> enabled{false};\nstatic std::atomic<size_t> active{0};\nstatic std::atomic_flag lock=ATOMIC_FLAG_INIT;\nstatic size_t overflow=0;\nstruct Stack { uintptr_t hash=0; void *pcs[16]{}; size_t frames=0, samples=0, bytes=0, size=0; };\nstatic Stack stacks[2048];\nstatic void report() {\n enabled.store(false);while(active.load()){}\n Dl_info info{};dladdr(reinterpret_cast<void *>(&report),&info);\n const auto base=reinterpret_cast<uintptr_t>(info.dli_fbase);\n std::array<size_t,2048> order{};for(size_t i=0;i<order.size();++i)order[i]=i;\n std::sort(order.begin(),order.end(),[](size_t a,size_t b){return stacks[a].bytes>stacks[b].bytes;});\n std::fprintf(stderr,"SAMPLED_SITES denominator=64 overflow=%zu\\n",overflow);\n for(size_t i=0;i<30 && stacks[order[i]].samples;++i){\n  const auto &s=stacks[order[i]];\n  std::fprintf(stderr,"ALLOC_SITE size=%zu samples=%zu sample_bytes=%zu",s.size,s.samples,s.bytes);\n  for(size_t j=0;j<s.frames;++j){const auto pc=reinterpret_cast<uintptr_t>(s.pcs[j]);if(pc>=base && pc-base<0x80000000ULL)std::fprintf(stderr," pc=%zx",pc-base);}\n  std::fprintf(stderr,"\\n");\n }\n}\n}\nextern "C" void __sanitizer_malloc_hook(const volatile void *p,size_t size){\n rss_probe::active.fetch_add(1);\n static thread_local uint32_t random=0x61c88647U;\n random^=random<<13;random^=random>>17;random^=random<<5;\n if(rss_probe::enabled.load() && (random&63U)==0U){\n  void *pcs[16]{};int thread=0;const auto frames=__asan_get_alloc_stack(const_cast<void *>(p),pcs,16,&thread);\n  uintptr_t hash=size+0x9e3779b9U;\n  for(size_t i=0;i<frames;++i)hash=(hash^reinterpret_cast<uintptr_t>(pcs[i]))*1099511628211ULL;\n  if(!hash)hash=1;\n  while(rss_probe::lock.test_and_set()){}\n  size_t slot=hash%2048U,probes=0;\n  for(;probes<2048U;++probes,slot=(slot+1U)%2048U){\n   auto &s=rss_probe::stacks[slot];\n   if(!s.hash || s.hash==hash){if(!s.hash){s.hash=hash;s.size=size;s.frames=frames;for(size_t i=0;i<frames;++i)s.pcs[i]=pcs[i];}++s.samples;s.bytes+=size;break;}\n  }\n  if(probes==2048U)++rss_probe::overflow;\n  rss_probe::lock.clear();\n }\n rss_probe::active.fetch_sub(1);\n}\n'
rel='tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp'
s=(root/rel).read_text()
s=prefix+allocationHooks+'extern "C" unsigned long __sanitizer_get_current_allocated_bytes();\nextern "C" unsigned long __sanitizer_get_heap_size();\n'+s
old='graph::query::QueryGuard guard(120000,64);graph::query::QueryContext context;context.guard=&guard;'
new='''{
        graph::parser::cypher::CypherParserImpl parser;
        (void)parser.parseToLogical("UNWIND range(1,20000) AS x WITH x ORDER BY x DESC RETURN collect(x) AS values");
        (void)engine.execute("RETURN 1 AS parser_warmup");
    }
    struct MemorySamples {
        size_t calls=0, baseline=0, peakRss=0, peakAllocated=0, peakHeap=0, lastAllocated=0;
        ~MemorySamples() { rss_probe::report(); std::fprintf(stderr, "REAL_RSS_PROBE calls=%zu baseline=%zu peak_rss=%zu growth=%zu peak_allocated=%zu heap=%zu last_allocated=%zu\\n",calls,baseline,peakRss,peakRss-baseline,peakAllocated,peakHeap,lastAllocated); }
    } samples;
    graph::query::QueryGuard guard(120000,64,[&] {
        const size_t rss=residentWithoutAllocation();
        if (!samples.calls++) { samples.baseline=rss; rss_probe::enabled.store(true); }
        samples.peakRss=std::max(samples.peakRss,rss);
        samples.lastAllocated=__sanitizer_get_current_allocated_bytes();
        samples.peakAllocated=std::max(samples.peakAllocated,samples.lastAllocated);
        samples.peakHeap=std::max(samples.peakHeap,static_cast<size_t>(__sanitizer_get_heap_size()));
        return rss;
    });graph::query::QueryContext context;context.guard=&guard;'''
assert old in s;s=s.replace(old,new,1);(out/'test.cpp').write_text(s)
env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
commands=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','CMakeFiles/zyx_test_suite.dir/'+rel+'.o'],cwd=build,text=True).splitlines()
args=shlex.split(next(x for x in commands if '-c '+str(root/rel) in x));newargs=[];i=0
while i<len(args):
 a=args[i]
 if a in ['-MF','-MT','-o']:i+=2;continue
 if a in ['-MD','-MMD']:i+=1;continue
 newargs.append(str(out/'test.cpp') if a==str(root/rel) else a);i+=1
newargs+=['-o',str(out/'test.o'),'-fsanitize=address','-fno-omit-frame-pointer']
with (out/'compile.log').open('w') as f: rc=subprocess.run(newargs,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
print('compile',rc,flush=True)
if rc:raise SystemExit(rc)
runtimeRel='src/query/execution/LogicalPlanReadRuntime.cpp'
runtimeCommands=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','CMakeFiles/zyx_core.dir/'+runtimeRel+'.o'],cwd=build,text=True).splitlines()
runtimeOriginal=shlex.split(next(x for x in runtimeCommands if '-c '+str(root/runtimeRel) in x));runtimeArgs=[];i=0
while i<len(runtimeOriginal):
 a=runtimeOriginal[i]
 if a in ['-MF','-MT','-o']:i+=2;continue
 if a in ['-MD','-MMD']:i+=1;continue
 runtimeArgs.append(a.replace('/workspace/zyx-ci-fix/include','/workspace/zyx-epoch-next/include'));i+=1
runtimeArgs+=['-o',str(out/'runtime-empty-reserve.o')]
with (out/'runtime-compile.log').open('w') as f:rc=subprocess.run(runtimeArgs,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
print('runtime compile',rc,flush=True)
if rc:raise SystemExit(rc)
args=shlex.split(subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=build,text=True).splitlines()[-1]);args=args[args.index('&&')+1:];args=args[:args.index('&&')]
args=[str(out/'test.o') if a=='CMakeFiles/zyx_test_suite.dir/'+rel+'.o' else a for a in args]
args.insert(1,str(out/'runtime-empty-reserve.o'));args[args.index('-o')+1]=str(out/'suite');args+=['-fsanitize=address','-fno-omit-frame-pointer']
with (out/'link.log').open('w') as f:rc=subprocess.run(args,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
print('link',rc,flush=True)
