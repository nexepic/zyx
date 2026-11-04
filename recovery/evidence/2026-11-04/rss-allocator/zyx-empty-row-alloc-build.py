from pathlib import Path
import subprocess,shlex,os
build=Path('/workspace/zyx-ci-config');main='/workspace/zyx-ci-fix';env=os.environ.copy();env['LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
rel='tests/src/query/execution/test_GuardedPropertyCopy.cpp'
cmds=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','CMakeFiles/zyx_test_suite.dir/'+rel+'.o'],cwd=build,text=True).splitlines();original=shlex.split(next(s for s in cmds if '-c '+main+'/'+rel in s))
link=shlex.split(subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=build,text=True).splitlines()[-1]);link=link[link.index('&&')+1:];link=link[:link.index('&&')]
for label,root in [('before',main),('after','/workspace/zyx-epoch-next')]:
 args=[];i=0
 while i<len(original):
  a=original[i]
  if a in ['-MF','-MT','-o']:i+=2;continue
  if a in ['-MD','-MMD']:i+=1;continue
  args.append('/tmp/zyx-empty-row-alloc.cpp' if a==main+'/'+rel else a.replace(main,root));i+=1
 obj='/tmp/zyx-empty-row-'+label+'.o';args+=['-o',obj]
 with open('/tmp/zyx-empty-row-'+label+'-compile.log','w') as out:rc=subprocess.run(args,cwd=build,env=env,stdout=out,stderr=subprocess.STDOUT).returncode
 print(label,'compile',rc,flush=True)
 if rc:continue
 args=[a for a in link if not a.endswith('.o')];args.insert(1,obj);args[args.index('-o')+1]='/tmp/zyx-empty-row-'+label
 with open('/tmp/zyx-empty-row-'+label+'-link.log','w') as out:rc=subprocess.run(args,cwd=build,env=env,stdout=out,stderr=subprocess.STDOUT).returncode
 print(label,'link',rc,flush=True)
 if not rc:
  env['LD_LIBRARY_PATH']=env['LIBRARY_PATH'];r=subprocess.run(['/tmp/zyx-empty-row-'+label],env=env,stdout=subprocess.PIPE,text=True);print(r.stdout,flush=True)
