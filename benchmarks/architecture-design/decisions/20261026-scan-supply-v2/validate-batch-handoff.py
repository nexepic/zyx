"""Selected query module regression against each full matching-profile core."""
from pathlib import Path
import hashlib,json,subprocess,time
D=Path(__file__).resolve().parent;R=Path('/workspace/zyx');receipts=[]
profiles=[('scan',47956,'build-scan-candidate',1,1),('hybrid',47973,'build-hybrid-compat',1,0),('row',47986,'build-scan-row-production',0,0)]
recipes=json.loads((D/'batch-handoff-tests/compile-recipes.json').read_text())
base=json.loads((D/'record-view-modules/integrated-link.json').read_text())
lib_index=next(i for i,x in enumerate(base) if x.endswith('build-scan-candidate/libzyx_core.a'))
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
for profile,pid,build,hybrid,scan in profiles:
 out=D/('batch-handoff-'+profile);out.mkdir(exist_ok=True);objects=[];commands=[]
 for index,recipe in enumerate(recipes):
  a=[x.replace('build-scan-candidate',build).replace('-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=1','-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT='+str(hybrid)).replace('-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=1','-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT='+str(scan)) for x in recipe]
  obj=out/Path(a[a.index('-o')+1]).name;a[a.index('-o')+1]=str(obj);objects.append(str(obj));commands.append(a)
  with (out/(str(index)+'-compile.log')).open('w') as log:subprocess.run(a,stdout=log,stderr=log,check=True)
 (out/'compile-recipes.json').write_text(json.dumps(commands,indent=2)+'\n')
 deadline=time.monotonic()+14400
 while Path('/proc',str(pid)).exists():
  assert time.monotonic()<deadline;time.sleep(2)
 log=Path('/tmp/zyx-batch-handoff-'+profile+'-final-build.log').read_text()
 assert 'Linking CXX static library libzyx_core.a' in log and 'build stopped:' not in log,log[-3000:]
 # Every compilation in this final build began after NodeSupplyAndSorting.inc stabilized.
 library=R/build/'libzyx_core.a';assert library.stat().st_mtime_ns>(R/'src/query/execution/read_runtime/NodeSupplyAndSorting.inc').stat().st_mtime_ns
 hook=D.parent/'20261026-scan-supply-v1'/('metadata-row/StorageIO.o' if profile=='row' else 'StorageIO.o')
 link=[base[0],str(hook),*objects]+[x.replace('build-scan-candidate',build) for x in base[lib_index:]]
 link[link.index('-o')+1]=str(out/'modules');(out/'link.json').write_text(json.dumps(link,indent=2)+'\n')
 with (out/'link.log').open('w') as log:subprocess.run(link,stdout=log,stderr=log,check=True)
 before=sha(library);cmd=[str(out/'modules'),'--gtest_output=json:'+str(out/'tests.json')]
 print('START',profile,'selected query tests',flush=True)
 with (out/'tests.log').open('w') as log:subprocess.run(cmd,stdout=log,stderr=log,check=True,timeout=1800)
 result=json.loads((out/'tests.json').read_text());assert result['failures']==0 and sha(library)==before
 assert any(x['name']=='LogicalPlanCertifiedBatchHandoffTest' and x['tests']==6 for x in result['testsuites'])
 receipt=dict(profile=profile,tests=result['tests'],failures=0,library_sha256=before,scope='three selected query module files, full matching core; no full unit suite')
 (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');receipts.append(receipt);print('PASS',profile,result['tests'],flush=True)
(D/'batch-handoff-compatibility-receipt.json').write_text(json.dumps(receipts,indent=2)+'\n')
