import subprocess,os
cc='/workspace/zyx-sysroot/usr/bin/clang++-19';env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
base=['-std=c++20','-O0','-g0','-I/workspace/zyx-ci-fix/include','-I/workspace/zyx-ci-fix/src','-isystem','/workspace/zyx-sysroot/usr/include']
objs=['/tmp/zyx-tree-focused/'+x+'.o' for x in ['PropertyTypes','PropertyValueCodec','Format','ChecksumUtils','PerfTrace']]
for label,extra in [('before',['-I/tmp/zyx-collect-baseline']),('after',[])]:
 subprocess.run([cc,*extra,*base,'/tmp/zyx-collect-bench.cpp',*objs,'-lz','-pthread','-o','/tmp/zyx-collect-'+label],env=env,check=True)
 with open('/tmp/zyx-collect-'+label+'.log','w') as f: subprocess.run(['/tmp/zyx-collect-'+label],stdout=f,check=True)
