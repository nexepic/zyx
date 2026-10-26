from pathlib import Path
import subprocess,json
p=Path('/workspace/zyx/benchmarks/architecture-design/decisions/20261026-s4-native-write-cost');r=json.loads((p/'test-recipes.json').read_text());subprocess.run(r['compile'],check=True)
cmd=r['link'].copy();i=next(i for i,x in enumerate(cmd) if x.endswith('/build-scan-candidate/libzyx_core.a'));cmd[i]='/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan/final-delivery/original-artifacts/B_library.a';cmd[-1]+='-before-cow';subprocess.run(cmd,check=True)
with (p/'before-cow-test.log').open('w') as f: result=subprocess.run([cmd[-1],'--gtest_filter=ScanReadProofTest.ExhaustedNativeRowPoolUsesFreshCowDestinationAndPreservesOldPin','--gtest_output=json:'+str(p/'before-cow-test.json')],stdout=f,stderr=subprocess.STDOUT)
assert result.returncode!=0,'Regression must fail on original S3 core'
subprocess.run(r['link'],check=True)
with (p/'after-cow-tests.log').open('w') as f:subprocess.run([r['link'][-1],'--gtest_filter=ScanReadProofTest.Native*:ScanReadProofTest.ExhaustedNativeRowPool*','--gtest_output=json:'+str(p/'after-cow-tests.json')],stdout=f,stderr=subprocess.STDOUT,check=True)
print('Original S3 failure confirmed; corrected native write boundary tests passed')
