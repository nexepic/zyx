from pathlib import Path
import json,subprocess,hashlib
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'logical-radix-targeted';dest.mkdir(exist_ok=True)
old=json.loads((root/'buffer-targeted/build.json').read_text())['public'];link=old['link'][:];link[link.index('-o')+1]=str(dest/'public-baseline');link=[a.replace('--dependency-file='+str(root/'buffer-targeted/public-link.d'),'--dependency-file='+str(dest/'public-link.d')) for a in link]
with (dest/'build.log').open('w') as log:subprocess.run(link,cwd=old['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
(dest/'build.json').write_text(json.dumps({'public':{'compile':old['compile'],'link':link,'cwd':old['cwd'],'source_sha256':old['source_sha256'],'binary_sha256':sha(dest/'public-baseline'),'reused_observer_object':str(root/'buffer-targeted/public.o'),'reason':'Only private LocationDirectory.cpp and CatalogTree.cpp logical COW materialization changed; observer source and all public headers are unchanged.'}},indent=2)+'\n')
