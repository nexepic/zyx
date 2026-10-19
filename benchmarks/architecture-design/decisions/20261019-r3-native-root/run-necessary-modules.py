"""Compile/link only native descriptor-root affected modules, for both formats."""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import sys
root=Path(__file__).resolve().parents[4]
b=Path(sys.argv[1]) if len(sys.argv)>1 else root/'buildDir'
r=Path(sys.argv[2]) if len(sys.argv)>2 else root/'benchmarks/architecture-design/results/20261019-r3-native-root'
r.mkdir(parents=True,exist_ok=True)
modules=['tests/src/storage/relocatable/property/test_HybridPropertyDescriptorStore.cpp', 'tests/src/storage/relocatable/property/test_HybridPropertyDescriptor.cpp', 'tests/src/storage/relocatable/format/test_Superblock.cpp', 'tests/src/storage/relocatable/format/test_SuperblockFile.cpp', 'tests/src/storage/relocatable/format/test_SuperblockPair.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataWriteBatch.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataPagePool.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataPagePoolRecovery.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataDirectoryEvacuation.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataSnapshotRegistry.cpp', 'tests/src/storage/relocatable/maintenance/test_RelocatableIntegrityScanner.cpp', 'tests/src/storage/relocatable/entity/test_NativeGraphWritePlanner.cpp', 'tests/src/storage/relocatable/entity/test_NativeGraphWritePlanner_PublicContracts.cpp']
paths=[root/p for p in modules]
jobs=os.environ.get("ZYX_SCOPED_TEST_JOBS", "4")
subprocess.run(['ninja','-C',str(b),'-j',jobs,'libzyx_core.a'],check=True)
commands=subprocess.check_output(['ninja','-C',str(b),'-t','commands','zyx_test_suite'],text=True).splitlines()
objects=[]
for p in paths:
 args=shlex.split(next(v for v in commands if ' -c '+str(p) in v));objects.append(args[args.index('-o')+1])
subprocess.run(['ninja','-C',str(b),'-j',jobs,*objects],check=True)
link=shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
link=[v for v in link if not v.endswith('.cpp.o')]
link[link.index('-o'):link.index('-o')]=objects
binary=r/'r3-native-root-tests';link[link.index('-o')+1]=str(binary)
link.append('/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a')
subprocess.run(link,cwd=b,check=True)
suites=sorted({m for p in paths for m in re.findall(r'\bTEST(?:_F)?\(\s*(\w+)\s*,',p.read_text())})
(r/'test-scope.json').write_text(json.dumps({'modules':modules,'suites':suites,'full_suite':False,'build_jobs':jobs,'hybrid_format_enabled':'ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT:BOOL=ON' in (b/'CMakeCache.txt').read_text(),'link':link},indent=2)+'\n')
subprocess.run([str(binary),'--gtest_filter='+':'.join(s+'.*' for s in suites),'--gtest_output=json:'+str(r/'results.json')],cwd=r,check=True)
