from pathlib import Path
import subprocess,shlex,json,os,hashlib
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/results/20261022-core-cost/cold-global-source-control';d.mkdir(exist_ok=True)
header=r/'include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp';s=header.read_text();a=s.index('''            // Removing global hot authority changes''');b=s.index('''            uint64_t requiredOwners = 0; size_t emitted = 0;''',a);s=s[:a]+s[b:]
override=d/'include';p=override/'graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(s)
cmd=shlex.split(Path('/tmp/native-cold-derived-core-commands.txt').read_text());cmd.insert(1,'-I'+str(override));obj=d/'without-source-auth.o'
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
(d/'compile.json').write_text(json.dumps(cmd,indent=2)+'\n')
with (d/'compile.log').open('w') as f:subprocess.run(cmd,cwd='/workspace/zyx-build-core-hybrid',stdout=f,stderr=subprocess.STDOUT,check=True)
raw=r/'benchmarks/architecture-design/results/20261022-core-cost/cold-block-preparation-hybrid';args=json.loads((raw/'link.json').read_text());exe=d/'without-source-auth-tests';args[args.index('-o')+1]=str(exe);args.insert(1,str(obj));args.insert(args.index('/workspace/zyx-build-core-hybrid/libzyx_core.a'),str(raw/'storageio-test-hooks.o'));args=[('--dependency-file='+str(d/'link.d')) if a.startswith('--dependency-file=') else a for a in args]
(d/'link.json').write_text(json.dumps(args,indent=2)+'\n')
with (d/'link.log').open('w') as f:subprocess.run(args,cwd=r/'buildDir',stdout=f,stderr=subprocess.STDOUT,check=True)
with (d/'tests.log').open('w') as f:run=subprocess.run([str(exe),'--gtest_filter=HybridPropertyColdCompatibleHotTest.GlobalDemotionCannotLaunderForbiddenHotTokenInUnrepackedColdOwner','--gtest_output=json:'+str(d/'tests.json')],stdout=f,stderr=subprocess.STDOUT,env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/cold-global-control-%p.profraw'))
j=json.loads((d/'tests.json').read_text());assert j['tests']==1 and j['failures']==1 and run.returncode==1
(d/'receipt.json').write_text(json.dumps({'deliberate_control':True,'difference':'only omit original cold-root group authentication at global hot-authority removal; override object precedes test object for weak inline selection','full_suite':False,'timing_benchmark':False,'progress_percent':90,'returncode':run.returncode,'header_sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_sha256':hashlib.file_digest(exe.open('rb'),'sha256').hexdigest(),'library_sha256':hashlib.file_digest(Path('/workspace/zyx-build-core-hybrid/libzyx_core.a').open('rb'),'sha256').hexdigest(),'expected_failure':'bad former-hot token in a separate unrepacked cold owner reaches completed private preparation (frames > 0)'},indent=2)+'\n');exe.unlink();print('without-source-auth control fails as expected; generated executable removed')
