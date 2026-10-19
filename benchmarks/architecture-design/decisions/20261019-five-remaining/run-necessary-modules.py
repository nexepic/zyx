"""Compile/link only R1.3/R2.3/R3.2/R4.3/R5.1 affected modules."""
from pathlib import Path
import json
import re
import shlex
import subprocess
import sys
root=Path(__file__).resolve().parents[4]
b=Path(sys.argv[1]) if len(sys.argv)>1 else root/'buildDir'
r=Path(sys.argv[2]) if len(sys.argv)>2 else root/'benchmarks/architecture-design/results/20261019-five-remaining'
r.mkdir(parents=True,exist_ok=True)
modules=['tests/src/query/execution/test_LogicalPlanSortGrowth.cpp', 'tests/src/query/execution/test_LogicalPlanBoundedTopK.cpp', 'tests/src/query/execution/test_CoordinatedMaterializedBudget.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataPageReadContext.cpp', 'tests/src/storage/relocatable/metadata/test_LocationDirectory.cpp', 'tests/src/storage/relocatable/metadata/test_LocationDirectory_RouteValidation.cpp', 'tests/src/storage/relocatable/metadata/test_LocationDirectoryScanCache.cpp', 'tests/src/storage/relocatable/metadata/test_MetadataSnapshotRegistry.cpp', 'tests/src/storage/relocatable/metadata/test_RelocatableMetadataStore_CompactionObservation.cpp', 'tests/src/storage/relocatable/maintenance/test_RelocatableCompactionObservation.cpp', 'tests/src/storage/relocatable/property/test_ReservedPropertySubblockTransition.cpp', 'tests/src/storage/relocatable/property/test_PropertySubblockCandidate.cpp', 'tests/src/storage/relocatable/property/test_PropertySubblockFieldTransition.cpp', 'tests/src/storage/relocatable/adjacency/test_AdjacencyTopologyContract.cpp']
paths=[root/p for p in modules]
subprocess.run(['ninja','-C',str(b),'-j','4','libzyx_core.a'],check=True)
commands=subprocess.check_output(['ninja','-C',str(b),'-t','commands','zyx_test_suite'],text=True).splitlines()
objects=[]
for p in paths:
 args=shlex.split(next(v for v in commands if ' -c '+str(p) in v));objects.append(args[args.index('-o')+1])
subprocess.run(['ninja','-C',str(b),'-j','4',*objects],check=True)
link=shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'))
link=[v for v in link if not v.endswith('.cpp.o')]
link[link.index('-o'):link.index('-o')]=objects
binary=r/'five-remaining-tests';link[link.index('-o')+1]=str(binary)
link.append('/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a')
subprocess.run(link,cwd=b,check=True)
suites=sorted({m for p in paths for m in re.findall(r'\bTEST(?:_F)?\(\s*(\w+)\s*,',p.read_text())})
(r/'test-scope.json').write_text(json.dumps({'modules':modules,'suites':suites,'full_suite':False,'link':link},indent=2)+'\n')
subprocess.run([str(binary),'--gtest_filter='+':'.join(s+'.*' for s in suites),'--gtest_output=json:'+str(r/'results.json')],cwd=r,check=True)
