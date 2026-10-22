from pathlib import Path
import json,subprocess
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/cpu-attribution';dest.mkdir(exist_ok=True)
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
compile=recipe['compile'];compile=[a.replace('-I/workspace/zyx', '-I/workspace/zyx-p0-profile-source', 1) if a.startswith('-I/workspace/zyx') and not a.startswith('-I/workspace/zyx-build') else a for a in compile]
for flag in ['-o','-MT','-MF']:
 compile[compile.index(flag)+1]=str(dest/('cpu-probe.d' if flag=='-MF' else 'cpu-probe.o'))
compile[-1]=str(source)
link=[str(dest/'cpu-probe.o') if a.endswith('/public.o') else str(dest/'cpu-probe') if a.endswith('/public-baseline') else '--dependency-file='+str(dest/'cpu-link.d') if a.startswith('--dependency-file=') else a for a in recipe['link']]
link+=['-ldl','-lrt']
with (dest/'build.log').open('w') as log:
 for cmd in [compile,link]:subprocess.run(cmd,cwd=recipe['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':compile,'link':link,'cwd':recipe['cwd'],'source_base':'5500a068','purpose':'exclusive CPU sampling; not latency benchmark'},indent=2)+'\n')
print('profile observer built')
