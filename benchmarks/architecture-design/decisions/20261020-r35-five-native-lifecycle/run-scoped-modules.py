import json,subprocess,os,sys
from pathlib import Path
mode=sys.argv[1];build='/workspace/zyx/buildDir' if mode=='default' else '/workspace/zyx-build-work-candidate'
r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
objects=['storage/relocatable/property/test_HybridPropertyReadContext.cpp','storage/relocatable/adjacency/test_CoveredAdjacencyCandidate.cpp','query/api/test_QueryEngineAnchorPlanning.cpp','query/api/test_QueryEngineLogicalBackend.cpp','query/optimization/test_LogicalPlanOptimizer.cpp','query/execution/test_LogicalGraphCursors.cpp','storage/relocatable/entity/test_PageBackedGraphReadAdapter.cpp','storage/relocatable/property/test_HybridPropertyOrdinaryConversion.cpp','storage/relocatable/entity/test_NativeGraphWritePlanner_PublicContracts.cpp','storage/relocatable/property/test_PropertySubblockDirectories.cpp','storage/relocatable/property/test_PropertySubblockMapMutation.cpp']
obj=['CMakeFiles/zyx_test_suite.dir/tests/src/'+p+'.o' for p in objects]
with open('/tmp/zyx-five-final-'+mode+'-build.log','w') as out: subprocess.run(['cmake','--build',build,'--target','zyx_core',*obj,'-j3'],stdout=out,stderr=subprocess.STDOUT,check=True)
template='range-default-final-link.json' if mode=='default' else 'range-candidate-link.json'
a=[v for v in json.load(open(r/template)) if not v.endswith('.o')];i=a.index('-o');a[i:i]=obj;a[a.index('-o')+1]=str(r/f'five-final-{mode}-tests');a=[('--dependency-file=/tmp/zyx-five-final-'+mode+'.d') if v.startswith('--dependency-file=') else v for v in a]
subprocess.run(a,cwd=build,check=True)
with open('/tmp/zyx-five-final-'+mode+'-tests.log','w') as out:p=subprocess.run([str(r/f'five-final-{mode}-tests'),'--gtest_output=json:'+str(r/f'five-final-{mode}-tests.json')],cwd=build,env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/zyx-five-final-%p.profraw'),stdout=out,stderr=subprocess.STDOUT)
print(mode,p.returncode,flush=True)
sys.exit(p.returncode)
