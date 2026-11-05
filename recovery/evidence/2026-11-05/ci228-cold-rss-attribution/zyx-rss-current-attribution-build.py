from pathlib import Path
import json,subprocess,os
root=Path('/workspace/zyx-ci-fix');base=Path('/workspace/zyx-ci228-full-asan-core');out=Path('/tmp/zyx-rss-current-attribution');out.mkdir(exist_ok=True)
manifest=json.loads((base/'final/tests-manifest.json').read_text());cmd=manifest['new_unit_compile'].copy()
source=(root/'tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp').read_text()
prefix=r'''
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sanitizer/allocator_interface.h>
#include <sanitizer/asan_interface.h>
namespace rss_attribution {
static std::atomic<bool> enabled{false};
static std::atomic<size_t> allocations{0},requested{0},frees{0},freed{0},sampled{0};
static thread_local bool inside=false;
struct Site{void *pc[16]{};size_t frames=0,bytes=0,count=0;};
static Site sites[2048];static std::atomic_flag lock=ATOMIC_FLAG_INIT;static size_t overflow=0;
void report(const char *phase){
 char data[8192];const int fd=::open("/proc/self/status",O_RDONLY);const auto n=fd<0?-1:::read(fd,data,sizeof(data));if(fd>=0)::close(fd);
 if(n>0)for(char *p=data;p<data+n;){char *e=static_cast<char*>(std::memchr(p,'\n',static_cast<size_t>(data+n-p)));if(!e)break;
 if(std::strncmp(p,"RssAnon:",8)==0||std::strncmp(p,"RssFile:",8)==0||std::strncmp(p,"RssShmem:",9)==0)std::fprintf(stderr,"RSS_COMPONENT %s %.*s\n",phase,static_cast<int>(e-p),p);p=e+1;}
 std::fprintf(stderr,"RSS_ALLOCATOR %s live=%zu heap=%zu free=%zu total_calls=%zu requested=%zu free_calls=%zu freed=%zu\n",phase,__sanitizer_get_current_allocated_bytes(),__sanitizer_get_heap_size(),__sanitizer_get_free_bytes(),allocations.load(),requested.load(),frees.load(),freed.load());
 std::fprintf(stderr,"RSS_MAP_PAUSE phase=%s\n",phase);std::fflush(stderr);::raise(SIGSTOP);
}
struct QueryScope{QueryScope(){report("before");enabled=true;}~QueryScope(){enabled=false;report("after");__asan_print_accumulated_stats();
 std::fprintf(stderr,"SITES sample_denominator=256 overflow=%zu\n",overflow);
 for(size_t rank=0;rank<25;++rank){size_t best=0;for(size_t i=1;i<2048;++i)if(sites[i].bytes>sites[best].bytes)best=i;auto &s=sites[best];if(!s.count)break;
 std::fprintf(stderr,"SITE count=%zu bytes=%zu",s.count,s.bytes);for(size_t i=0;i<s.frames;++i)std::fprintf(stderr," pc=%p",s.pc[i]);std::fprintf(stderr,"\n");s.bytes=0;}
}};
}
extern "C" void __sanitizer_malloc_hook(const volatile void *p,size_t size){using namespace rss_attribution;if(!enabled.load()||inside)return;inside=true;allocations.fetch_add(1);requested.fetch_add(size);
 if((sampled.fetch_add(1)&255U)==0){void *pcs[16]{};int tid=0;const size_t frames=__asan_get_alloc_stack(const_cast<void*>(p),pcs,16,&tid);size_t hash=frames;for(size_t i=0;i<frames;++i)hash=hash*1099511628211ULL+reinterpret_cast<uintptr_t>(pcs[i]);while(lock.test_and_set()){};size_t slot=hash%2048,tries=0;for(;tries<2048;++tries,slot=(slot+1)%2048){auto &s=sites[slot];if(!s.count||(s.frames==frames&&std::memcmp(s.pc,pcs,frames*sizeof(void*))==0)){if(!s.count){s.frames=frames;std::memcpy(s.pc,pcs,frames*sizeof(void*));}++s.count;s.bytes+=size;break;}}if(tries==2048)++overflow;lock.clear();}
 inside=false;}
extern "C" void __sanitizer_free_hook(const volatile void *p){using namespace rss_attribution;if(!enabled.load()||inside||!p)return;inside=true;frees.fetch_add(1);freed.fetch_add(__sanitizer_get_allocated_size(const_cast<void*>(p)));inside=false;}
'''
prefix=prefix.replace('fd<0?-1:::read','fd<0 ? -1 : ::read')
needle='graph::query::QueryGuard guard(120000,64);graph::query::QueryContext context;context.guard=&guard;'
assert source.count(needle)==1;source=prefix+source.replace(needle,'''rss_attribution::QueryScope attributionScope;size_t sampledBaseline=0;bool sampledFirst=true;bool reported=false;graph::query::QueryGuard guard(120000,64,[&](){const size_t now=graph::query::QueryGuard::getCurrentRSSBytes();if(sampledFirst){sampledFirst=false;sampledBaseline=now;}else if(!reported&&now>sampledBaseline+64U*1024U*1024U){reported=true;rss_attribution::report("rejection-before-unwind");__asan_print_accumulated_stats();}return now;});graph::query::QueryContext context;context.guard=&guard;''')
p=out/'probe.cpp';p.write_text(source)
old=cmd[cmd.index('-c')+1];cmd[cmd.index('-c')+1]=str(p);cmd[cmd.index('-o')+1]=str(out/'probe.o');cmd+=['-fno-pie']
env={**os.environ,'LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'}
with (out/'compile.log').open('w') as f:r=subprocess.run(cmd,cwd='/workspace/zyx-ci-config',env=env,stdout=f,stderr=subprocess.STDOUT)
print('compile',r.returncode,flush=True)
if r.returncode:raise SystemExit(r.returncode)
args=manifest['link'].copy();args=[x for x in args if not x.endswith('/test_RelocatableQueryScratch.o') and not x.endswith('/test_ResultPropertyMapAdmission.o')];args.insert(1,str(out/'probe.o'));args[args.index('-o')+1]=str(out/'suite');args+=['-no-pie']
with (out/'link.log').open('w') as f:r=subprocess.run(args,cwd='/workspace/zyx-ci-config',env=env,stdout=f,stderr=subprocess.STDOUT)
(out/'manifest.json').write_text(json.dumps({'production_source':'561460b30174e3dbc167f7a87bfceae6e1e7f138','diagnostic_source_only':str(p),'compile':cmd,'link':args,'link_exit':r.returncode},indent=2)+'\n');print('link',r.returncode,flush=True)
raise SystemExit(r.returncode)
