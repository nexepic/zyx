"""Rebuild public probes from preserved recipes; this script performs no timings."""
from pathlib import Path
import json
import subprocess
import sys
REPO=Path('/workspace/zyx');HERE=Path(__file__).resolve().parent
OLD=HERE.parent/'20261025-final-scan-comparison'
OUT=REPO/'benchmarks/architecture-design/results/20261024-work-scan/s3-lifecycle'
OUT.mkdir(exist_ok=True)
for name,recipe,source in [('public-b','public-b',None),('public-legacy','public-a',None),('values-b','value-b',None),('generality','value-b',HERE.parent/'20261025-scan-generality/lifecycle.cpp')]:
 if len(sys.argv)>1 and name not in sys.argv[1:]:continue
 old=json.loads((OLD/(recipe+'-recipes.json')).read_text());old_output=old['link'][old['link'].index('-o')+1];dest=str(OUT/name);commands={}
 for kind in ['compile','link']:
  args=[x.replace(old_output,dest) for x in old[kind]]
  if kind=='compile' and source is not None:args[args.index('-c')+1]=str(source)
  commands[kind]=args
  subprocess.run(args,check=True)
 (HERE/(name+'-recipes.json')).write_text(json.dumps(commands,indent=2)+'\n')
 print(name,'built (no query timing)',flush=True)

if 'control' in sys.argv[1:]:
 old=json.loads((OLD/'public-a-recipes.json').read_text());old_output=old['link'][old['link'].index('-o')+1];dest=str(OUT/'public-control');commands={}
 frozen=Path('/workspace/zyx-frozen-control-369039d')
 for kind in ['compile','link']:
  args=[]
  for value in old[kind]:
   value=value.replace(old_output,dest)
   if value.startswith('-I'+str(REPO)):
    value=value.replace(str(REPO),str(frozen)).replace('build-scan-row-production','build')
   elif value.endswith('/build-scan-row-production/libzyx_core.a'):value=str(frozen/'build/libzyx_core.a')
   elif value.endswith('/build-scan-row-production/libzyx_inputxx.a'):value=str(frozen/'build/libzyx_inputxx.a')
   args.append(value)
  commands[kind]=args;subprocess.run(args,check=True)
 (HERE/'control-recipes.json').write_text(json.dumps(commands,indent=2)+'\n')
 print('Frozen 369 control built (no query timing)',flush=True)
