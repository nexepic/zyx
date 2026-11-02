import shlex,subprocess,pathlib,sys
wd='/workspace/zyx-unified-build';root='/workspace/zyx-unified-fixtures/';base='/workspace/zyx-latestonly/'
cmds=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=wd,text=True).splitlines()
files=subprocess.check_output(['git','diff','--name-only','15263106','e668fa19'],cwd=root,text=True).splitlines()
files=[x for x in files if (x.startswith('src/') or x.startswith('tests/')) and x.endswith('.cpp')]
for i,rel in enumerate(files,1):
 matches=[x for x in cmds if x.endswith(' -c '+base+rel)]
 if not matches:print('MISSING',rel,flush=True);sys.exit(1)
 a=shlex.split(matches[0]);
 for flag in ('-MT','-MF','-o'):
  n=a.index(flag);del a[n:n+2]
 if '-MD' in a:a.remove('-MD')
 a.remove('-c');a.insert(1,'-fsyntax-only');a.insert(1,'-I'+root+'tests/include');a.insert(1,'-I'+root+'include');a.insert(1,'-I'+root+'src');a.insert(1,'-I'+root);a[-1]=root+rel
 p=subprocess.run(a,cwd=wd,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
 print(f'{i}/{len(files)}',rel,'OK' if p.returncode==0 else 'FAILED',flush=True)
 if p.returncode:print(p.stderr,flush=True);sys.exit(p.returncode)
