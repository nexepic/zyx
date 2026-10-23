from pathlib import Path
import subprocess,os,shlex,json
root=Path('/workspace/zyx');obj='CMakeFiles/zyx_core.dir/src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp.o'
for profile,folder in [('hybrid','zyx-build-core-hybrid'),('default','zyx-build-core-candidate')]:
 build=Path('/workspace')/folder
 cmd=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands',obj],cwd=build,text=True).splitlines()[-1]
 with open('/tmp/cold-repack-'+profile+'-core-build.log','w') as log:
  subprocess.run(shlex.split(cmd),cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['/workspace/zyx-sysroot/usr/bin/llvm-ar-19','r','libzyx_core.a',obj],cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run(['python3','/tmp/build-cold-row-codec-'+profile+'-modules.py'],cwd=root,check=True)
 d=root/'benchmarks/architecture-design/results/20261022-core-cost'/('cold-row-codec-'+profile)
 args=json.loads((d/'link.json').read_text())
 with (d/'repack-link.log').open('w') as log:subprocess.run(args,cwd=root/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
 with (d/'repack-tests.log').open('w') as log:
  subprocess.run([str(d/'module-tests'),'--gtest_filter=HybridPropertyColdRowBlockTest.*:HybridPropertyColdRowBindingsTest.*','--gtest_output=json:'+str(d/'repack-tests.json')],env=dict(os.environ,LLVM_PROFILE_FILE='/tmp/cold-repack-'+profile+'-%p.profraw'),stdout=log,stderr=subprocess.STDOUT,check=True)
 print(profile,'passed',flush=True)
