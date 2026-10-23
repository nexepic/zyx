from pathlib import Path
import subprocess,json,os,hashlib,shlex
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/results/20261022-core-cost/cold-row-ownership-default'
cmd=shlex.split(Path('/tmp/short-text-default-storageio-compile.txt').read_text());cmd.insert(1,'-DZYX_ENABLE_STORAGE_TEST_HOOKS=1');obj=d/'storageio-test-hooks.o'
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
with (d/'test-hook-build.log').open('w') as log:subprocess.run(cmd,cwd='/workspace/zyx-build-core-candidate',stdout=log,stderr=subprocess.STDOUT,check=True)
args=json.loads((d/'link.json').read_text());args.insert(args.index('/workspace/zyx-build-core-candidate/libzyx_core.a'),str(obj))
with (d/'link.log').open('w') as log:subprocess.run(args,cwd=r/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
with (d/'tests.log').open('w') as log:p=subprocess.run([str(d/'module-tests'),'--gtest_filter=*','--gtest_output=json:'+str(d/'tests.json')],env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/short-text-hybrid-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
(d/'receipt.json').write_text(json.dumps({'full_suite':False,'returncode':p.returncode,'library_sha256':sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a')),'default_profile':'5/31','progress_percent':90,'test_hook_compile':cmd,'link':args,'test_hook_source_sha256':sha(r/'src/storage/StorageIO.cpp'),'test_hook_purpose':'current StorageIO implementation with native syscall fault-injection seams; no production algorithm change or timing claim'},indent=2)+'\n');print(p.returncode);raise SystemExit(p.returncode)
