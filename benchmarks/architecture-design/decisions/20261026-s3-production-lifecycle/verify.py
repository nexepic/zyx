"""Compile/run affected modules only; never build or run the full test suite."""
from pathlib import Path
import json
import os
import subprocess
import sys
REPO=Path('/workspace/zyx')
HERE=Path(__file__).resolve().parent
OUT=REPO/'benchmarks/architecture-design/results/20261024-work-scan/s3-lifecycle'
OUT.mkdir(exist_ok=True)
CXX='/workspace/zyx-sysroot/usr/bin/clang++-19'
PROFILES={'candidate':('build-scan-candidate',1,1),'default':('build-scan-row-production',0,0),'compat':('build-hybrid-compat',1,0)}
COMMON=['-std=c++20','-O0','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-missing-field-initializers','-Wno-missing-designated-field-initializers','-DZYX_HAS_LZ4=1','-DZYX_HAS_ZSTD=1','-I'+str(REPO/'include'),'-I'+str(REPO/'src'),'-I'+str(REPO/'tests/include'),'-isystem','/workspace/zyx-sysroot/usr/include']
MODULES={'candidate':['entity/test_BuildingScanMembership.cpp','entity/test_ScanReadProof.cpp','entity/test_BuildingScanIndexPacking.cpp','property/test_PropertyCollectionRootStoreCache.cpp'],'default':['property/test_PropertyCollectionRootStoreCache.cpp'],'compat':['property/test_HybridPropertyReadContext.cpp','property/test_HybridPropertyColumnReadReuse.cpp','property/test_PropertyCollectionRootStoreCache.cpp']}
def main(profile):
 build,hybrid,scan=PROFILES[profile];objects=[];commands=[]
 for relative in MODULES[profile]:
  source=REPO/'tests/src/storage/relocatable'/relative;obj=OUT/(profile+'-'+source.stem+'.o');objects.append(str(obj))
  cmd=[CXX,*COMMON,f'-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT={hybrid}',f'-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT={scan}','-I'+str(REPO/build),'-c',str(source),'-o',str(obj)];commands.append(cmd)
  # Existing compatibility fixtures intentionally discard nodiscard results.
  if profile=='compat':cmd.insert(cmd.index('-c'),'-Wno-unused-result')
  subprocess.run(cmd,check=True)
 binary=OUT/(profile+'-tests')
 link=[CXX,*objects,str(REPO/build/'libzyx_core.a'),'-L/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu','-lgtest_main','-lgtest','-lz','-llz4','-lxxhash','-lzstd','-lboost_filesystem','-lboost_atomic','/workspace/antlr4-install/lib/libantlr4-runtime.a','-pthread','-o',str(binary)]
 subprocess.run(link,check=True)
 (HERE/(profile+'-recipes.json')).write_text(json.dumps({'compile':commands,'link':link},indent=2)+'\n')
 result=HERE/(profile+'-tests.json')
 with (HERE/(profile+'-tests.log')).open('w') as stream:
  subprocess.run([str(binary),'--gtest_output=json:'+str(result)],stdout=stream,stderr=subprocess.STDOUT,check=True)
 evidence=json.loads(result.read_text());assert evidence['tests']>0 and evidence['failures']==0
 print(profile,evidence['tests'],'tests passed; no full suite',flush=True)
if __name__=='__main__':main(sys.argv[1])
