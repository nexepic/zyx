from pathlib import Path
import json,subprocess,hashlib,shutil
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';old=root/'post-buffer-cpu';dest=root/'post-buffer-index-cpu';dest.mkdir(exist_ok=True);shutil.copyfile(old/'cpu-phase.hpp',dest/'cpu-phase.hpp')
s=(repo/'src/storage/relocatable/index/BTreePageStore.cpp').read_text()
for signature,phase in [('void apply(const BTreeScalarIndexOperation &operation)',4),('BTreeStructuralMutationPlan buildPlan()',5),('BTreePage materialize(const OverlayMutationNode &node',8)]:
 start=s.index(signature);brace=s.index('{',start);s=s[:brace+1]+f'\n            cpu_phase::Scope cpuPhase({phase});'+s[brace+1:]
s='#include "cpu-phase.hpp"\n'+s;source=dest/'BTreePageStore.cpp';source.write_text(s)
recipe=json.loads((old/'build.json').read_text());cmd=recipe['compile'][0][:]
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(dest/('BTreePageStore.d' if flag=='-MF' else 'BTreePageStore.o'))
cmd[-1]=str(source);link=recipe['link'][:];link[link.index('-o')+1]=str(dest/'cpu-probe');link=[str(a).replace('--dependency-file='+str(old/'cpu-link.d'),'--dependency-file='+str(dest/'cpu-link.d')) for a in link];link.insert(link.index('libzyx_core.a'),str(dest/'BTreePageStore.o'))
with (dest/'build.log').open('w') as log:
 for command in [cmd,link]:subprocess.run(command,cwd=recipe['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':cmd,'link':link,'cwd':recipe['cwd'],'base_observer':str(old/'build.json'),'purpose':'exclusive overlay apply/build/materialization CPU ownership; production source unchanged','additional_phases':{'4':'BTree overlay apply','5':'BTree overlay build including previews','8':'BTree materialization outside planning'},'library_sha256':recipe['library_sha256']},indent=2)+'\n');print('index phase observer built')
