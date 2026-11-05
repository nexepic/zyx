from pathlib import Path
import os,subprocess
root=Path('/workspace/zyx-ci-fix');cc='/workspace/zyx-sysroot/usr/bin/clang++-19';env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
base=['-std=c++20','-O0','-g0','-I'+str(root/'include'),'-I'+str(root/'src'),'-isystem','/workspace/zyx-sysroot/usr/include']
Path('/tmp/zyx-codec-append-before.cpp').write_text(subprocess.check_output(['git','show','4374b7ff:src/storage/relocatable/property/PropertyValueCodec.cpp'],cwd=root,text=True))
objects=['/tmp/zyx-tree-focused/'+n+'.o' for n in ['PropertyTypes','Format','ChecksumUtils','PerfTrace']]
for label,source in [('before','/tmp/zyx-codec-append-before.cpp'),('after',str(root/'src/storage/relocatable/property/PropertyValueCodec.cpp'))]:
 obj='/tmp/zyx-codec-append-'+label+'.o';subprocess.run([cc,*base,'-c',source,'-o',obj],env=env,check=True)
 target='/tmp/zyx-codec-append-'+label
 subprocess.run([cc,*base,'/tmp/zyx-codec-append-requests.cpp',obj,*objects,'-pthread','-lz','-o',target],env=env,check=True)
 with open(target+'.log','w') as f:subprocess.run([target],stdout=f,check=True)
