import shlex,json,subprocess
from pathlib import Path
R=Path('/workspace/zyx');O=R/'benchmarks/architecture-design/decisions/20261027-column-block/key-scratch'
commands=[]
for name,path in [('runtime','/tmp/zyx-runtime-command.txt'),('test','/tmp/zyx-aggregate-test-command.txt')]:
 c=shlex.split(Path(path).read_text());c=[x for x in c if x not in ['-g','-fprofile-instr-generate','-fcoverage-mapping']]
 c=[x.replace('ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=0','ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=1').replace('ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=0','ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=1') for x in c]
 for flag,suffix in [('-o','.o'),('-MT','.o'),('-MF','.d')]:c[c.index(flag)+1]=str(O/(name+suffix))
 if name=='test':c+=['-O1']
 commands.append(c)
link=json.loads((R/'benchmarks/architecture-design/decisions/20261026-scan-supply-v2/b-whole-group-deletion/link-command.json').read_text());link=[x for x in link if not x.endswith('.o')];link[1:1]=[str(O/'runtime.o'),str(O/'test.o')];link[link.index('-o')+1]=str(O/'module');link.insert(link.index('-pthread'),str(R/'build-scan-candidate/libzyx_inputxx.a'));commands.append(link)
for i,c in enumerate(commands):
 (O/f'command-{i}.json').write_text(json.dumps(c,indent=2)+'\n')
 with (O/f'build-{i}.log').open('w') as out:r=subprocess.run(c,cwd=R,stdout=out,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError((O/f'build-{i}.log').read_text()[-6000:])
print('built selected aggregate module')
