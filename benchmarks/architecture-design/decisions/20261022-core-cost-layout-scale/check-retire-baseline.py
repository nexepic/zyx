from pathlib import Path
import json,subprocess,os,hashlib
r=Path('/workspace/zyx');out=r/'benchmarks/architecture-design/decisions/20261022-core-cost-layout-scale';d=r/'benchmarks/architecture-design/results/20261022-core-cost/short-text-hybrid'
a=json.loads((d/'link.json').read_text());a=[x for x in a if 'test_HybridPropertyLayoutScale.o' not in x];a.insert(a.index('/workspace/zyx-build-core-hybrid/libzyx_core.a'),str(d/'storageio-test-hooks.o'))
lib=Path('/workspace/zyx-short-text-hybrid-candidate/libzyx_core.a');a=[str(lib) if x=='/workspace/zyx-build-core-hybrid/libzyx_core.a' else x for x in a];a[a.index('-o')+1]=str(out/'frozen-retire-checks');subprocess.run(a,cwd=r/'buildDir',check=True)
f='RelocatableMetadataPagePoolTest.DeferredPublicationRetainsFramesUntilBoundedAllocationThreshold:RelocatableMetadataPagePoolTest.RecoveryReclaimsDeferredUnpublishedFrames'
with (out/'frozen-retire-checks.log').open('w') as log:p=subprocess.run([str(out/'frozen-retire-checks'),'--gtest_filter='+f,'--gtest_output=json:'+str(out/'frozen-retire-checks.json')],env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/retire-baseline-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
j=json.loads((out/'frozen-retire-checks.json').read_text());(out/'frozen-retire-checks-receipt.json').write_text(json.dumps({'library':str(lib),'sha256':hashlib.file_digest(lib.open('rb'),'sha256').hexdigest(),'returncode':p.returncode,'tests':j['tests'],'failures':j['failures'],'test_filter':f,'full_suite':False},indent=2)+'\n');print(j['tests'],j['failures'])
