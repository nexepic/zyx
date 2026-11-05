from pathlib import Path
import subprocess,os
root=Path('/workspace/zyx-ci-fix'); cc='/workspace/zyx-sysroot/usr/bin/clang++-19';out=Path('/tmp/zyx-tree-focused');out.mkdir(exist_ok=True)
tests=['test_LogicalAggregateAccumulator','test_SelectionVector','test_BoundedBatch','test_ExecutionBindingContainers','test_AdmissionPmrContainers','test_QuerySpillRowCodec','test_ExternalQuerySort','test_BudgetMemoryResource','test_PrepaidMemoryResource','test_DynamicValueArena','test_PrimitiveNumericColumns']
sources=[str(root/'tests/src/query/execution'/f'{x}.cpp') for x in tests]+[str(root/'tests/src/storage/relocatable/property/test_PropertyValueCodec_Bytes.cpp')]+[str(root/x) for x in ['src/core/PropertyTypes.cpp','src/storage/relocatable/property/PropertyValueCodec.cpp','src/storage/relocatable/format/Format.cpp','src/utils/ChecksumUtils.cpp','src/debug/PerfTrace.cpp','src/query/QueryContext.cpp','src/query/QueryGuard.cpp']]+['/workspace/zyx-sysroot/usr/src/googletest/googletest/src/gtest-all.cc','/workspace/zyx-sysroot/usr/src/googletest/googletest/src/gtest_main.cc']
flags=['-std=c++20','-O0','-g0','-I'+str(root/'include'),'-I'+str(root/'src'),'-isystem','/workspace/zyx-sysroot/usr/include','-I/workspace/zyx-sysroot/usr/src/googletest/googletest/include','-I/workspace/zyx-sysroot/usr/src/googletest/googletest']
env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
for source in sources:
 obj=out/(Path(source).stem+'.o')
 with (out/(Path(source).stem+'.log')).open('w') as f:r=subprocess.run([cc,*flags,'-c',source,'-o',str(obj)],env=env,stdout=f,stderr=subprocess.STDOUT)
 print(Path(source).name,r.returncode,flush=True)
 if r.returncode:raise SystemExit(r.returncode)
with (out/'link.log').open('w') as f:r=subprocess.run([cc,*flags,*[str(out/(Path(source).stem+'.o')) for source in sources],'-pthread','-lz','-o',str(out/'suite')],env=env,stdout=f,stderr=subprocess.STDOUT)
print('link',r.returncode,flush=True);raise SystemExit(r.returncode)
