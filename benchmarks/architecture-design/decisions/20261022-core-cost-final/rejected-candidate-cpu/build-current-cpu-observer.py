from pathlib import Path
import json,subprocess
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/current-cpu';dest.mkdir(exist_ok=True)
recipe=json.loads((repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/build.json').read_text())['public']
s=(repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/public-probe.cpp').read_text()
s='#include "cpu-sampler.hpp"\n'+s
needle='   const auto start=timing?Clock::now():Clock::time_point{};'
assert needle in s
s=s.replace(needle,'   cpu_sample::begin();\n'+needle,1)
needle='   const double ms=timing?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;'
assert needle in s
s=s.replace(needle,needle+'\n   cpu_sample::end();',1)
source=dest/'cpu-probe.cpp';source.write_text(s)
compile=[a.replace('/workspace/zyx-build-r7-row','/workspace/zyx-build-core-candidate') for a in recipe['compile']]
for flag in ['-o','-MT','-MF']:
 compile[compile.index(flag)+1]=str(dest/('cpu-probe.d' if flag=='-MF' else 'cpu-probe.o'))
compile[-1]=str(source)
link=[str(dest/'cpu-probe.o') if a.endswith('/public.o') else str(dest/'cpu-probe') if a.endswith('/public-baseline') else '--dependency-file='+str(dest/'cpu-link.d') if a.startswith('--dependency-file=') else a for a in recipe['link']]
link+=['-ldl','-lrt']
with (dest/'build.log').open('w') as log:
 for cmd in [compile,link]:subprocess.run(cmd,cwd='/workspace/zyx-build-core-candidate',stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':compile,'link':link,'cwd':'/workspace/zyx-build-core-candidate','source_base':'P7 frozen rejected candidate','purpose':'exclusive CPU sampling; not latency benchmark'},indent=2)+'\n')
print('profile observer built')
