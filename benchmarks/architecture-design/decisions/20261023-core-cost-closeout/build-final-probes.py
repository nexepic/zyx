from pathlib import Path
import json,subprocess,hashlib
repo=Path('/workspace/zyx');p0=repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0';root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'closeout';dest.mkdir(exist_ok=True)
recipes={}
for name,old in json.loads((p0/'build.json').read_text()).items():
 compile=[a.replace('/workspace/zyx-build-r7-row','/workspace/zyx-build-core-candidate') for a in old['compile']]
 for flag in ['-o','-MT','-MF']:compile[compile.index(flag)+1]=str(dest/(name+('.d' if flag=='-MF' else '.o')))
 link=[a.replace(str(root)+'/',str(dest)+'/') for a in old['link']]
 cwd='/workspace/zyx-build-core-candidate'
 with (dest/(name+'-build.log')).open('w') as out:
  for cmd in [compile,link]:subprocess.run(cmd,cwd=cwd,stdout=out,stderr=subprocess.STDOUT,check=True)
 recipes[name]={'compile':compile,'link':link,'cwd':cwd,'source_sha256':hashlib.file_digest(Path(compile[-1]).open('rb'),'sha256').hexdigest(),'binary_sha256':hashlib.file_digest(Path(link[link.index('-o')+1]).open('rb'),'sha256').hexdigest()}
(dest/'build.json').write_text(json.dumps(recipes,indent=2)+'\n');print('formal probes built from unchanged P0 harness sources')
