from pathlib import Path
import subprocess,json,shlex,os,hashlib
repo=Path('/workspace/zyx');build=Path('/workspace/zyx-build-core-hybrid');d=repo/'benchmarks/architecture-design/results/20261022-core-cost/cold-row-codec-hybrid'
recipe=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-C',str(build),'-t','commands','CMakeFiles/zyx_core.dir/src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp.o'],text=True)
base=shlex.split([line for line in recipe.splitlines() if ' -c ' in line and 'HybridPropertyColdRowBlock.cpp' in line][-1]);test=json.loads((d/'compile.json').read_text())['commands'][0]
commands=[]
for args,obj,cwd in [(base,d/'no-lz4-core.o',build),(test,d/'no-lz4-tests.o',repo/'buildDir')]:
 args=[a.replace('-DZYX_HAS_LZ4=1','-DZYX_HAS_LZ4=0') for a in args];assert '-DZYX_HAS_LZ4=0' in args
 for flag in ['-o','-MT','-MF']:
  if flag in args:args[args.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
 with (d/(obj.stem+'-build.log')).open('w') as log:subprocess.run(args,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,check=True)
 commands.append({'cwd':str(cwd),'command':args})
args=json.loads((d/'link.json').read_text());args=[str(d/'no-lz4-tests.o') if a.endswith('property-test_HybridPropertyColdRowBlock.o') else a for a in args];args.insert(args.index('/workspace/zyx-build-core-hybrid/libzyx_core.a'),str(d/'no-lz4-core.o'));args[args.index('-o')+1]=str(d/'no-lz4-tests')
with (d/'no-lz4-link.log').open('w') as log:subprocess.run(args,cwd=repo/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
with (d/'no-lz4-tests.log').open('w') as log:p=subprocess.run([str(d/'no-lz4-tests'),'--gtest_output=json:'+str(d/'no-lz4-tests.json')],env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/cold-row-no-lz4-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
(d/'no-lz4-receipt.json').write_text(json.dumps({'returncode':p.returncode,'full_suite':False,'codec_macro':'ZYX_HAS_LZ4=0','scope':'Cold codec and its tests compiled without LZ4; rest of existing engine archive unchanged, not a complete no-LZ4 engine build','compile':commands,'link':args,'source_sha256':hashlib.sha256((repo/'src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp').read_bytes()).hexdigest()},indent=2)+'\n');print(p.returncode);raise SystemExit(p.returncode)
