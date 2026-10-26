"""Selected offline membership, index packing and source-proof modules only."""
from pathlib import Path
import json,subprocess
D=Path(__file__).resolve().parent;R=D.parents[3];O=D/'scan';O.mkdir(exist_ok=True)
a=json.loads((D/'base-recipe.json').read_text());objects=[str(D/'StorageIO.o')];commands=[]
for name in ['BuildingScanMembership','BuildingScanIndexPacking','ScanReadProof']:
 c=a['compile'][0].copy();c+=['-DZYX_ENABLE_STORAGE_TEST_HOOKS=1']
 c[c.index('-c')+1]=str(R/f'tests/src/storage/relocatable/entity/test_{name}.cpp')
 obj=str(O/f'{name}.o');c[c.index('-o')+1]=obj;objects.append(obj);commands.append(c)
link=[x for x in a['link'] if not x.endswith('.o')];link[1:1]=objects;link[link.index('-o')+1]=str(O/'modules');commands.append(link)
(O/'recipes.json').write_text(json.dumps(commands,indent=2)+'\n')
with (O/'build.log').open('w') as log:
 for c in commands:
  print('BUILD',c[c.index('-c')+1] if '-c' in c else 'link',flush=True)
  subprocess.run(c,stdout=log,stderr=subprocess.STDOUT,check=True,cwd=R)
