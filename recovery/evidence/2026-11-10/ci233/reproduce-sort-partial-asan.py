import json,shlex,subprocess,pathlib
root=pathlib.Path('/workspace/zyx-ci-reconstructed'); build=root/'buildDir'
commands=json.load(open(build/'compile_commands.json')); objects=[]
with open('/tmp/zyx-233-sort-asan-build.log','w') as log:
 for name in ['test_ExternalQuerySort.cpp','test_QuerySpillRowCodec.cpp']:
  e=next(e for e in commands if e['file'].endswith('/'+name)); cmd=e.get('arguments') or shlex.split(e['command'])
  cmd=list(cmd); output='/tmp/zyx-233-'+name+'.asan.o'; cmd[cmd.index('-o')+1]=output
  cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,cwd=e['directory'],stdout=log,stderr=subprocess.STDOUT,check=True);objects.append(output)
 raw=subprocess.check_output(['ninja','-t','commands','zyx_test_suite'],cwd=build,text=True).splitlines()[-1]
 parts=shlex.split(raw); start=parts.index('&&')+1; end=parts.index('&&',start); cmd=parts[start:end]
 cleaned=[]; i=0
 while i<len(cmd):
  a=cmd[i]
  if a=='-Xlinker' and i+1<len(cmd) and cmd[i+1].startswith('--dependency-file='): i+=2;continue
  if a.endswith('.o'): i+=1;continue
  if a=='-o':cleaned+=['-o','/tmp/zyx-233-sort-asan'];i+=2;continue
  cleaned.append(a);i+=1
 cleaned[1:1]=objects
 cleaned+=['/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a','-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(cleaned,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
