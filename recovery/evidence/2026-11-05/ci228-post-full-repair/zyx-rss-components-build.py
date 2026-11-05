from pathlib import Path
import subprocess,os,shlex
root=Path('/workspace/zyx-ci-fix');build=Path('/workspace/zyx-ci-config');out=Path('/workspace/zyx-ci228-full-asan-core');env={**os.environ,'LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'}
rel='tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp'
s=subprocess.check_output(['git','show','ff02643c:'+rel],cwd=root,text=True)
prefix='''#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
extern "C" void __asan_print_accumulated_stats();
namespace {
void reportRssComponents(const char *phase) {
 char bytes[4096]{}; const int fd=::open("/proc/self/status",O_RDONLY);
 if(fd<0)return;const ssize_t size=::read(fd,bytes,sizeof(bytes)-1);::close(fd);
 if(size<=0)return;
 for(char *line=bytes;line<bytes+size;){char *end=static_cast<char *>(std::memchr(line,'\\n',static_cast<size_t>(bytes+size-line)));if(!end)break;
  if(std::strncmp(line,"RssAnon:",8)==0 || std::strncmp(line,"RssFile:",8)==0 || std::strncmp(line,"RssShmem:",9)==0)
   std::fprintf(stderr,"RSS_COMPONENT %s %.*s\\n",phase,static_cast<int>(end-line),line);
  line=end+1;
 }
}
struct ReportFinalRss {~ReportFinalRss(){reportRssComponents("after-query");__asan_print_accumulated_stats();}};
}
'''
needle='graph::query::QueryGuard guard(120000,64);graph::query::QueryContext context;context.guard=&guard;'
assert needle in s;s=s.replace(needle,'reportRssComponents("before-query");ReportFinalRss finalRss;'+needle,1)
source=out/'rss-components.cpp';source.write_text(prefix+s)
lines=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','CMakeFiles/zyx_test_suite.dir/'+rel+'.o'],cwd=build,text=True).splitlines();args=shlex.split(next(x for x in lines if '-c '+str(root/rel) in x));cmd=[];i=0
while i<len(args):
 if args[i] in ['-o','-MF','-MT']:i+=2;continue
 if args[i] in ['-MD','-MMD','-fprofile-instr-generate','-fcoverage-mapping']:i+=1;continue
 cmd.append(str(source) if args[i]==str(root/rel) else args[i]);i+=1
obj=out/'rss-components.o';cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-o',str(obj)]
with (out/'rss-components-build.log').open('w') as f:r=subprocess.run(cmd,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT)
print('compile',r.returncode,flush=True)
if r.returncode:raise SystemExit(r.returncode)
s=Path('/tmp/zyx-ci228-full-asan-tests-relink.py').read_text();s=s.replace("str(out/('tests/'+Path(rel).stem+'.o'))","str(out/'rss-components.o') if 'QueryScratch' in rel else str(out/('tests/'+Path(rel).stem+'.o'))");s=s.replace("out/'native_tests'","out/'rss-components'");exec(compile(s,'<rss-link>','exec'))
