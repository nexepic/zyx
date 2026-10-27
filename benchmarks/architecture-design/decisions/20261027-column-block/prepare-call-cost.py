from pathlib import Path
import json,subprocess
N=Path('/workspace/zyx/benchmarks/architecture-design/decisions/20261027-column-block');B=N/'current-boundary';O=N/'call-cost'; assert not (O/'protocol.json').exists()
compile=['/workspace/zyx-sysroot/usr/bin/clang++-19','-O3','-DNDEBUG','-std=c++20','-I/workspace/zyx/include','-I/workspace/zyx-sysroot/usr/include','-c',str(O/'wrap.cpp'),'-o',str(O/'wrap.o')]
link=json.loads((B/'command-2.json').read_text());link=[x.replace(str(B/'probe-link.d'),str(O/'probe-link.d')).replace(str(B/'probe'),str(O/'probe')) if x in (str(B/'probe'),'-Xlinker') or 'dependency-file=' in x else x for x in link]
# Reuse the unchanged boundary probe object and production core.
link += [str(O/'wrap.o'),'-Wl,--wrap=fstat','-Wl,--wrap=crc32','-Wl,--wrap=_ZN5graph5query10QueryGuard9checkSlowEv']
for i,cmd in enumerate([compile,link]):
 (O/f'command-{i}.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with (O/f'build-{i}.log').open('w') as out:res=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT)
 if res.returncode:raise RuntimeError((O/f'build-{i}.log').read_text()[-4000:])
print('built linked-call diagnostic; production objects unchanged')
