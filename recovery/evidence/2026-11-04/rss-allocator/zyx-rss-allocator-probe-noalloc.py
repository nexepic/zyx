from pathlib import Path
import os,shlex,subprocess
root=Path('/workspace/zyx-ci-fix');build=Path('/workspace/zyx-ci-config');out=Path('/tmp/zyx-rss-allocator-probe');out.mkdir(exist_ok=True)
prefix = r"""
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
rel='tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp'
s=(root/rel).read_text()
s=prefix+'extern "C" unsigned long __sanitizer_get_current_allocated_bytes();\nextern "C" unsigned long __sanitizer_get_heap_size();\n'+s
old='graph::query::QueryGuard guard(120000,64);graph::query::QueryContext context;context.guard=&guard;'
new='''struct MemorySamples {
        size_t calls=0, baseline=0, peakRss=0, peakAllocated=0, peakHeap=0, lastAllocated=0;
        ~MemorySamples() { std::fprintf(stderr, "REAL_RSS_PROBE calls=%zu baseline=%zu peak_rss=%zu growth=%zu peak_allocated=%zu heap=%zu last_allocated=%zu\\n",calls,baseline,peakRss,peakRss-baseline,peakAllocated,peakHeap,lastAllocated); }
    } samples;
    graph::query::QueryGuard guard(120000,64,[&] {
        const size_t rss=residentWithoutAllocation();
        if (!samples.calls++) samples.baseline=rss;
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
args=shlex.split(subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=build,text=True).splitlines()[-1]);args=args[args.index('&&')+1:];args=args[:args.index('&&')]
args=[str(out/'test.o') if a=='CMakeFiles/zyx_test_suite.dir/'+rel+'.o' else a for a in args]
args[args.index('-o')+1]=str(out/'suite');args+=['-fsanitize=address','-fno-omit-frame-pointer']
with (out/'link.log').open('w') as f:rc=subprocess.run(args,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
print('link',rc,flush=True)
