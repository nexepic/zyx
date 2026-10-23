from pathlib import Path
import json,subprocess,hashlib
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'read-runs-targeted';dest.mkdir(exist_ok=True)
old=json.loads((root/'buffer-targeted/build.json').read_text())['public'];cmd=old['compile'][:]
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(dest/('public.d' if flag=='-MF' else 'public.o'))
link=old['link'][:];link=[str(dest/'public.o') if a==str(root/'buffer-targeted/public.o') else a.replace('--dependency-file='+str(root/'buffer-targeted/public-link.d'),'--dependency-file='+str(dest/'public-link.d')) for a in link];link[link.index('-o')+1]=str(dest/'public-baseline')
with (dest/'build.log').open('w') as log:
 for a in [cmd,link]:subprocess.run(a,cwd=old['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
(dest/'build.json').write_text(json.dumps({'public':{'compile':cmd,'link':link,'cwd':old['cwd'],'source_sha256':sha(Path(cmd[-1])),'binary_sha256':sha(dest/'public-baseline'),'library_sha256':sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a')),'reason':'unchanged P0 harness recompiled against current public headers and new virtual run interface; frozen old binary untouched'}},indent=2)+'\n')
