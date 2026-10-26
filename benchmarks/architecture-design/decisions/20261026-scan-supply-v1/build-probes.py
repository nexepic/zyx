from pathlib import Path
import json,subprocess
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent
for name in ('public-b','values-b'):
 a=json.loads((D.parent/'20261026-s3-production-lifecycle'/f'{name}-recipes.json').read_text())
 old=a['link'][a['link'].index('-o')+1];dest=str(D/name)
 for key in ('compile','link'):
  a[key]=[v.replace(old,dest) for v in a[key]]
 (D/f'{name}-recipes.json').write_text(json.dumps(a,indent=2)+'\n')
 with (D/f'{name}-build.log').open('w') as log:
  for key in ('compile','link'):subprocess.run(a[key],stdout=log,stderr=subprocess.STDOUT,check=True,cwd=R)
