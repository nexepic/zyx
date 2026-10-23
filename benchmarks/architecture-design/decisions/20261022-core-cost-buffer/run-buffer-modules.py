from pathlib import Path
import json,os,subprocess,hashlib
repo=Path('/workspace/zyx');out=repo/'benchmarks/architecture-design/results/20261022-core-cost';build=repo/'buildDir'
units=['storage/relocatable/cache/test_BufferPool']
objs=['CMakeFiles/zyx_test_suite.dir/tests/src/'+name+'.cpp.o' for name in units]
with (out/'buffer-modules-build.log').open('w') as log:
 subprocess.run(['/workspace/zyx-tools/bin/cmake','--build',str(build),'--target','zyx_core',*objs,'-j2'],stdout=log,stderr=subprocess.STDOUT,check=True)
args=json.loads((repo/'benchmarks/architecture-design/results/20261020-work-resume/range-default-final-link.json').read_text());args=[a for a in args if not a.endswith('.o')];i=args.index('-o');args[i:i]=objs;args[args.index('-o')+1]=str(out/'buffer-module-tests')
args=['/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libxxhash.so.0.8.3' if a=='-lxxhash' else '--dependency-file='+str(out/'route-module-link.d') if a.startswith('--dependency-file=') else a for a in args]
subprocess.run(args,cwd=build,check=True)
with (out/'buffer-module-tests.log').open('w') as log:
 p=subprocess.run([str(out/'buffer-module-tests'),'--gtest_filter=*','--gtest_output=json:'+str(out/'buffer-module-tests.json')],env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/core-route-module-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
(out/'buffer-module-receipt.json').write_text(json.dumps({'units':units,'full_suite':False,'returncode':p.returncode,'library_sha256':hashlib.file_digest((build/'libzyx_core.a').open('rb'),'sha256').hexdigest()},indent=2)+'\n');print('result',p.returncode,flush=True);raise SystemExit(p.returncode)
