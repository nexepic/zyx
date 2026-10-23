from pathlib import Path
import json,shlex,subprocess,os,hashlib
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/results/20261022-core-cost/native-cold-derived-control';d.mkdir(exist_ok=True)
p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp';s=p.read_text();start=s.index('''                if (found == oldValues.end()) {
                    // Derived indexes''');end=s.index('''                return found->second ?''',start)
s=s[:start]+'''                if (found == oldValues.end())
                    found = oldValues.emplace(token, roots.readEntityPropertyOptional(source, owner, token)).first;
'''+s[end:]
before=d/'RelocatableMetadataStoreHybridMutation-before.cpp';before.write_text(s)
cmd=shlex.split(Path('/tmp/native-cold-derived-core-commands.txt').read_text());obj=d/'before.o'
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
cmd[-1]=str(before);(d/'compile.json').write_text(json.dumps(cmd,indent=2)+'\n')
with (d/'compile.log').open('w') as f:subprocess.run(cmd,cwd='/workspace/zyx-build-core-hybrid',stdout=f,stderr=subprocess.STDOUT,check=True)
raw=r/'benchmarks/architecture-design/results/20261022-core-cost/cold-block-preparation-hybrid'
args=json.loads((raw/'link.json').read_text());exe=d/'before-tests';args[args.index('-o')+1]=str(exe)
args=[('--dependency-file='+str(d/'link.d')) if a.startswith('--dependency-file=') else a for a in args]
pos=args.index('/workspace/zyx-build-core-hybrid/libzyx_core.a');args[pos:pos]=[str(obj),str(raw/'storageio-test-hooks.o')]
(d/'link.json').write_text(json.dumps(args,indent=2)+'\n')
with (d/'link.log').open('w') as f:subprocess.run(args,cwd=r/'buildDir',stdout=f,stderr=subprocess.STDOUT,check=True)
with (d/'tests.log').open('w') as f:run=subprocess.run([str(exe),'--gtest_filter=HybridPropertyColdCompatibleHotTest.NativeCoordinatorUsesColdSourceForRealCatalogIndexesAndRequiredFields','--gtest_output=json:'+str(d/'tests.json')],stdout=f,stderr=subprocess.STDOUT,env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/native-cold-derived-before-%p.profraw'))
j=json.loads((d/'tests.json').read_text());assert j['tests']==1 and j['failures']==1 and run.returncode==1
(d/'receipt.json').write_text(json.dumps({'deliberate_before_fix_control':True,'full_suite':False,'timing_benchmark':False,'progress_percent':90,'returncode':run.returncode,'source_sha256':hashlib.file_digest(before.open('rb'),'sha256').hexdigest(),'binary_sha256':hashlib.file_digest(exe.open('rb'),'sha256').hexdigest(),'baseline_library_sha256':hashlib.file_digest(Path('/workspace/zyx-build-core-hybrid/libzyx_core.a').open('rb'),'sha256').hexdigest(),'expected_failure':'Constraint violation for unchanged cold field: old derived-source route reads empty durable ROW source instead of candidate source'},indent=2)+'\n')
exe.unlink();print('before-fix control fails as expected; generated executable removed, source/commands/reports/hashes retained')
