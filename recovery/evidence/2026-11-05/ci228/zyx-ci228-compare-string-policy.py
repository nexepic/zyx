from pathlib import Path
import subprocess,os
root=Path('/workspace/zyx-ci-fix');shadow=Path('/tmp/zyx-ci228-string-baseline');p=shadow/'graph/utils/StandardContainerHeapBytes.hpp';p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(subprocess.check_output(['git','show','HEAD:include/graph/utils/StandardContainerHeapBytes.hpp'],cwd=root))
lib='/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/lib';env=os.environ.copy();env['LIBRARY_PATH']=lib+':/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu';env['LD_LIBRARY_PATH']=env['LIBRARY_PATH']
for mode in ('before','after'):
 args=['/workspace/zyx-sysroot/usr/bin/clang++-19','-std=c++20','-O0','-g0','-nostdinc++','-isystem','/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/include/c++/v1','-stdlib=libc++','-L'+lib]
 if mode=='before':args+=['-I'+str(shadow)]
 args+=['-Iinclude','-Isrc','/tmp/zyx-ci228-string-budget.cpp','src/storage/relocatable/property/PropertyValueCodec.cpp','src/storage/relocatable/format/Format.cpp','src/utils/ChecksumUtils.cpp','src/debug/PerfTrace.cpp','-lz','-o','/tmp/zyx-ci228-string-budget-'+mode]
 with open('/tmp/zyx-ci228-string-budget-'+mode+'-build.log','w') as f:rc=subprocess.run(args,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
 print(mode,'compile',rc,flush=True)
 if rc:raise SystemExit(rc)
 result=subprocess.run(['/tmp/zyx-ci228-string-budget-'+mode],env=env,capture_output=True,text=True)
 Path('/tmp/zyx-ci228-string-budget-'+mode+'.log').write_text(result.stdout+result.stderr)
 print(mode,'exit',result.returncode,result.stdout,flush=True)
 assert result.returncode==(1 if mode=='before' else 0)
