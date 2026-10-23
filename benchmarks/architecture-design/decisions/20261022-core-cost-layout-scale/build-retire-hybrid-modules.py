from pathlib import Path
import json,subprocess,shlex,os,hashlib
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/short-text-hybrid';dest.mkdir(exist_ok=True)
base=shlex.split(Path('/tmp/short-text-record-compile.txt').read_text());commands=[];objects=[]
units=['metadata/test_CatalogRetireQueueReclaimer','metadata/test_MetadataRetirement','metadata/test_MetadataPublicationReclaimBatch','metadata/test_RelocatableMetadataStoreIdentity','property/test_HybridPropertyLayoutScale']
with (dest/'build-tests.log').open('w') as log:
 for unit in units:
  obj=dest/(unit.replace('/','-')+'.o');cmd=base[:]
  for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
  cmd[-1]=str(repo/'tests/src/storage/relocatable'/(unit+'.cpp'))
  cmd=[a.replace('-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=0','-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=1') for a in cmd]
  headers_latest=max(p.stat().st_mtime for root in ['include','src','tests/include'] for p in (repo/root).rglob('*.hpp'))
  if not obj.exists() or obj.stat().st_mtime<max(headers_latest,Path(cmd[-1]).stat().st_mtime):
   subprocess.run(cmd,cwd=repo/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
  objects.append(str(obj));commands.append(cmd)
(dest/'compile.json').write_text(json.dumps({'units':units,'commands':commands,'cwd':str(repo/'buildDir'),'full_suite':False},indent=2)+'\n')
args=json.loads((repo/'benchmarks/architecture-design/results/20261020-work-resume/range-default-final-link.json').read_text());args=[a for a in args if not a.endswith('.o')];idx=args.index('-o');args[idx:idx]=objects;args[args.index('-o')+1]=str(dest/'module-tests')
args=['/workspace/zyx-build-core-hybrid/libzyx_core.a' if a=='libzyx_core.a' else '/workspace/zyx-build-core-hybrid/libzyx_inputxx.a' if a=='libzyx_inputxx.a' else '/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libxxhash.so.0.8.3' if a=='-lxxhash' else '--dependency-file='+str(dest/'link.d') if a.startswith('--dependency-file=') else a for a in args]
(dest/'link.json').write_text(json.dumps(args,indent=2)+'\n')
print('module objects ready',flush=True)
