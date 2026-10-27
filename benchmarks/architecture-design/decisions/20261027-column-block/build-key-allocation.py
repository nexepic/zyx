from pathlib import Path
import subprocess,json
R=Path('/workspace/zyx');N=R/'benchmarks/architecture-design/decisions/20261027-column-block';O=N/'key-scratch';D=N.parent/'20261026-scan-supply-v2'
c=json.loads((N/'current-cpu/command-0.json').read_text())
for flag,suffix in [('-o','.o'),('-MT','.o'),('-MF','.d')]:c[c.index(flag)+1]=str(O/('allocation'+suffix))
c[c.index('-c')+1]=str(D/'allocation-probe.cpp');(O/'allocation-compile.json').write_text(json.dumps(c,indent=2)+'\n')
with (O/'allocation-compile.log').open('w') as f:r=subprocess.run(c,cwd=R,stdout=f,stderr=subprocess.STDOUT)
if r.returncode:raise RuntimeError((O/'allocation-compile.log').read_text()[-3000:])
print('compiled allocation probe')
