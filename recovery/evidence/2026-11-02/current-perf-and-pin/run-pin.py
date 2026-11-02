from pathlib import Path
import subprocess,tempfile
r=Path('/workspace/zyx-link-tmp/pin-release-evidence');r.mkdir(exist_ok=False)
for i in range(1,7):
 with tempfile.TemporaryDirectory(prefix='db-',dir=r) as d:
  cmd=['taskset','-c','0','/workspace/zyx-link-tmp/pin-release-space',str(Path(d)/'db.zyx')];p=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  (r/f'round{i}.log').write_text(f'command={cmd!r}\nexit={p.returncode}\n{p.stdout}\n{p.stderr}')
  if p.returncode:raise SystemExit(f'failed round{i}')
print('six rounds passed')
