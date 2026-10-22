from pathlib import Path
import json,subprocess
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/layout-prototype-release';dest.mkdir(exist_ok=True)
recipe=json.loads((repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/build.json').read_text())['public']
s=Path('/tmp/zyx-p5-layout-prototype.cpp').read_text();source=dest/'probe.cpp';source.write_text(s)
compile=recipe['compile'][:]
for flag in ['-o','-MT','-MF']:compile[compile.index(flag)+1]=str(dest/('probe.d' if flag=='-MF' else 'probe.o'))
compile[-1]=str(source);compile=[a.replace('/workspace/zyx-build-r7-row','/workspace/zyx-build-core-candidate') for a in compile]
link=[str(dest/'probe.o') if a.endswith('/public.o') else str(dest/'probe') if a.endswith('/public-baseline') else '--dependency-file='+str(dest/'link.d') if a.startswith('--dependency-file=') else a for a in recipe['link']]
link=['/workspace/zyx-build-core-candidate/libzyx_core.a' if a=='libzyx_core.a' else a for a in link]
with (dest/'build.log').open('w') as log:
 for cmd in [compile,link]:subprocess.run(cmd,cwd=repo/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':compile,'link':link,'cwd':str(repo/'buildDir'),'purpose':'copy/borrow counters only; Debug candidate, no latency comparison'},indent=2)+'\n')
print('built diagnostic')
