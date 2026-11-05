from pathlib import Path
import subprocess,os
root=Path('/workspace/zyx-ci-fix');p=Path('/tmp/zyx-ci228-layout-baseline/query/execution/BoundColumnLayout.hpp');p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(subprocess.check_output(['git','show','HEAD:src/query/execution/BoundColumnLayout.hpp'],cwd=root))
for abi in ('gnu','libcpp'):
 for version in ('before','after'):
  env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu';env['LD_LIBRARY_PATH']=env['LIBRARY_PATH']
  cmd=['/workspace/zyx-sysroot/usr/bin/clang++-19','-std=c++20','-O0','-g0']
  if abi=='libcpp':
   lib='/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/lib';env['LIBRARY_PATH']=lib+':'+env['LIBRARY_PATH'];env['LD_LIBRARY_PATH']=env['LIBRARY_PATH'];cmd+=['-nostdinc++','-isystem','/tmp/zyx-libcxx-refund/sysroot/usr/lib/llvm-19/include/c++/v1','-stdlib=libc++','-L'+lib]
  if version=='before':cmd+=['-I/tmp/zyx-ci228-layout-baseline']
  exe='/tmp/zyx-ci228-layout-'+abi+'-'+version
  cmd+=['-Iinclude','-Isrc','/tmp/zyx-ci228-layout-measure.cpp','src/debug/PerfTrace.cpp','-o',exe]
  with open(exe+'-build.log','w') as f:r=subprocess.run(cmd,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
  if r.returncode:raise SystemExit(r.returncode)
  r=subprocess.run([exe],cwd=root,env=env,capture_output=True,text=True);Path(exe+'.log').write_text(r.stdout+r.stderr);print(abi,version,'exit',r.returncode,r.stdout,flush=True);assert r.returncode==(1 if version=='before' else 0)
