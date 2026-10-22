from pathlib import Path
import subprocess,shlex,json,os
repo=Path('/workspace/zyx');build=repo/'buildDir';out=repo/'benchmarks/architecture-design/results/20261022-core-cost';unit='CMakeFiles/zyx_test_suite.dir/tests/src/query/execution/test_LogicalPlanFilterSelectedColumns.cpp.o'
raw=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands',unit],cwd=build,text=True).splitlines()[-1];args=shlex.split(raw)
args.insert(1,'-I/workspace/zyx-p0-profile-source/include');args[args.index('-o')+1]=str(out/'baseline-filter-test.o');args[args.index('-MF')+1]=str(out/'baseline-filter-test.d')
with (out/'baseline-filter-build.log').open('w') as log:subprocess.run(args,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
args=json.loads((repo/'benchmarks/architecture-design/results/20261020-work-resume/range-default-final-link.json').read_text());args=[a for a in args if not a.endswith('.o')];i=args.index('-o');args[i:i]=[str(out/'baseline-filter-test.o')];args[args.index('-o')+1]=str(out/'baseline-filter-test')
args=['/workspace/zyx-build-r7-row/libzyx_core.a' if a=='libzyx_core.a' else '/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libxxhash.so.0.8.3' if a=='-lxxhash' else '--dependency-file='+str(out/'baseline-filter-link.d') if a.startswith('--dependency-file=') else a for a in args]
subprocess.run(args,cwd=build,check=True)
with (out/'baseline-filter-test.log').open('w') as log:
 p=subprocess.run([str(out/'baseline-filter-test'),'--gtest_filter=LogicalPlanDirectEdgeRuntimeTest.ReversedSingleHopAnchorPreservesDirectionalBagsAndEarlyStop'],env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu',LLVM_PROFILE_FILE='/tmp/core-baseline-filter-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
print('baseline return',p.returncode)
