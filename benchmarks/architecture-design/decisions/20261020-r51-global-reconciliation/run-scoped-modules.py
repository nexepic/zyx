import json,subprocess,os,sys
from pathlib import Path
mode=sys.argv[1];build='/workspace/zyx/buildDir' if mode=='default' else '/workspace/zyx-build-work-candidate';r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
obj=['CMakeFiles/zyx_test_suite.dir/tests/src/storage/relocatable/'+p+'.cpp.o' for p in ['adjacency/test_CoveredAdjacencyCandidate','metadata/test_LocationDirectory']]
with open('/tmp/zyx-r51-global-'+mode+'-verified-build.log','w') as out:subprocess.run(['cmake','--build',build,'--target','zyx_core',*obj,'-j3'],stdout=out,stderr=subprocess.STDOUT,check=True)
template='range-default-final-link.json' if mode=='default' else 'range-candidate-link.json';a=[v for v in json.load(open(r/template)) if not v.endswith('.o')];i=a.index('-o');a[i:i]=obj;a[a.index('-o')+1]=str(r/f'r51-global-{mode}-tests');a=[('--dependency-file=/tmp/zyx-r51-global-'+mode+'.d') if v.startswith('--dependency-file=') else v for v in a];subprocess.run(a,cwd=build,check=True)
with open('/tmp/zyx-r51-global-'+mode+'-tests.log','w') as out:p=subprocess.run([str(r/f'r51-global-{mode}-tests'),'--gtest_output=json:'+str(r/f'r51-global-{mode}-tests.json')],cwd=build,env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/zyx-r51-global-%p.profraw'),stdout=out,stderr=subprocess.STDOUT)
print(mode,p.returncode,flush=True);sys.exit(p.returncode)
