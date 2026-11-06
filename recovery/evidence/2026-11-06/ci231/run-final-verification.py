from pathlib import Path
import os, subprocess, json, re, hashlib, gzip, shutil
repo=Path('/workspace/zyx-ci-fix'); evidence=repo/'recovery/evidence/2026-11-06/ci231'
env=os.environ.copy()
env.update(PATH='/tmp/zyx-ci228-tools:/workspace/zyx-tools/bin:/workspace/zyx-sysroot/usr/bin:'+env.get('PATH',''), LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu', LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu', CMAKE_BUILD_PARALLEL_LEVEL='2', PYTHONPATH='/tmp/zyx-ci-pytest', TMPDIR='/workspace/zyx-ci230-test-tmp', ZYX_TEST_KEEP_COVERAGE_ARTIFACTS='true')
sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
assert not subprocess.check_output(['git','diff','--name-only','HEAD'],cwd=repo,text=True).strip(), 'tracked changes must be saved before verification'
patterns=['BufferPoolTest.*','MetadataPageReadContextTest.*','RelocatableEntityLabelSetStoreTest.*','RelocatableEntityLabelSetStoreContractTest.*','PageBackedGraphReadAdapterTest.*','NativeReadSessionTest.*','AdmissionPmrContainersTest.*','OwnedLazyUnorderedMapTest.*','LogicalVectorPropertyBatchTest.*','LogicalGdsResourceOwnershipTest.*','QueryResultResourcesTest.*','LogicalPlanLoadCsvRuntimeTest.*']
vector_file=repo/'tests/src/storage/relocatable/backend/test_RelocatableStorageBackendSchema_Vector.cpp'
patterns += [a+'.'+b for a,b in re.findall(r'TEST_F\((\w+),\s*(\w+)\)',vector_file.read_text())]
def phase(name,args):
 e=env.copy(); e['ZYX_TEST_RUN_ID']=name; e['ZYX_TEST_XML_DIR']='/workspace/'+name+'-xml'; Path(e['ZYX_TEST_XML_DIR']).mkdir(exist_ok=True)
 if name.endswith('-full'): e['ZYX_TEST_SHARDS']='4'
 log=Path('/workspace')/(name+'.log'); print('START',name,'source',sha,flush=True)
 with log.open('w') as f:p=subprocess.run(args,cwd=repo,env=e,stdout=f,stderr=subprocess.STDOUT)
 print('FINISH',name,'exit',p.returncode,flush=True)
 if p.returncode: raise SystemExit(p.returncode)
 return log,Path(e['ZYX_TEST_XML_DIR'])
phase('zyx-ci231-final-target',['./scripts/run_tests.sh','--quick','--gtest-filter',':'.join(patterns),'--file','src/query/execution/procedure_runtime/BufferedRows.inc'])
# A second pass must find no stale object after edits made during the earlier
# exploratory build. This check runs after the final-source target succeeded.
p=subprocess.run(['cmake','--build','buildDir','--target','zyx_test_suite'],cwd=repo,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
Path('/workspace/zyx-ci231-no-work.log').write_text(p.stdout);print(p.stdout,flush=True)
if p.returncode:raise SystemExit(p.returncode)
binary=repo/'buildDir/zyx_test_suite'
e=env.copy();e['TMPDIR']='/workspace/zyx-ci231-long-tmp/'+('p'*200); e['LLVM_PROFILE_FILE']='/workspace/zyx-ci231-csv-long-green-%p.profraw'
with open('/workspace/zyx-ci231-csv-long-green.log','w') as f:p=subprocess.run([str(binary),'--gtest_filter=LogicalPlanLoadCsvRuntimeTest.ExecutionBudgetRejectsCsvSourceBuffersBeforeRowDelivery','--gtest_output=xml:/workspace/zyx-ci231-csv-long-green.xml'],cwd=repo,env=e,stdout=f,stderr=subprocess.STDOUT)
print('LONG PATH CSV',p.returncode,flush=True)
if p.returncode:raise SystemExit(p.returncode)
discovery=subprocess.check_output([str(binary),'--gtest_list_tests'],cwd=repo,env=env)
with gzip.open(evidence/'final-discovery.txt.gz','wb') as f:f.write(discovery)
products=[]
for path in [binary,repo/'buildDir/libzyx_core.a']:
 products.append({'path':str(path.resolve()),'bytes':path.stat().st_size,'sha256':hashlib.file_digest(path.open('rb'),'sha256').hexdigest()})
(evidence/'verified-products.json').write_text(json.dumps({'source_commit':sha,'platform':'Linux x86_64 Debian 13.6','products':products,'configuration':'Debug -O0 -g0; Clang 19; coverage ON'},indent=2)+'\n')
log,xml=phase('zyx-ci231-final-full',['./scripts/run_tests.sh','--quick'])
for product in products:
 assert hashlib.file_digest(Path(product['path']).open('rb'),'sha256').hexdigest()==product['sha256'], 'product changed during final full run'
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()==sha
assert not subprocess.check_output(['git','diff','--name-only','HEAD'],cwd=repo,text=True).strip()
for i in range(4):
 with gzip.open(evidence/f'final-shard-{i}.xml.gz','wb') as f:f.write((xml/f'shard-{i}.xml').read_bytes())
with gzip.open(evidence/'final-full.log.gz','wb') as f:f.write(log.read_bytes())
shutil.copy2(repo/'buildDir/coverage/test-shards-result.json',evidence/'final-supervisor.json')
with (evidence/'final-full-exact-once.json').open('w') as f:p=subprocess.run(['python3',str(evidence/'verify-exact-once.py')],cwd=repo,env=env,stdout=f,stderr=subprocess.STDOUT)
print('EXACT ONCE',p.returncode,flush=True);raise SystemExit(p.returncode)
