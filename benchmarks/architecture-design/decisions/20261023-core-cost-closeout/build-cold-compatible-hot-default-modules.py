from pathlib import Path
import json,subprocess,shlex,os,hashlib
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/cold-row-ownership-default';dest.mkdir(exist_ok=True)
base=shlex.split(Path('/tmp/short-text-record-compile.txt').read_text());commands=[];objects=[]
units=['property/test_HybridPropertyColdNativeLifecycle', 'property/test_HybridPropertyColdCompatibleHot', 'property/test_HybridPropertyColdConversionProbe', 'property/test_HybridPropertyOrdinaryConversion', 'property/test_HybridPropertyLargeFieldPreparation', 'property/test_PropertyCollectionRootBorrowed', 'property/test_PropertyCollectionRoot', 'property/test_HybridPropertyLargeOwnership', 'maintenance/test_LargeValueReachabilityReclaimer', 'maintenance/test_LargeValueMaintenanceJournal', 'property/test_HybridPropertyColdRowOwnership', 'property/test_HybridPropertyColdRowRouting', 'property/test_HybridPropertyColdRowInspection', 'property/test_HybridPropertyDescriptorStore', 'property/test_HybridPropertyColumnMembership', 'property/test_HybridPropertyPageImage', 'property/test_HybridPropertyPageBinding', 'property/test_HybridPropertyRecords', 'format/test_Superblock']
prior = json.loads((dest/'compile.json').read_text()) if (dest/'compile.json').exists() else {}
prior_commands = {c[-1]: c for c in prior.get('commands', [])}
def needs_compile(obj, command):
 if not obj.exists() or prior_commands.get(command[-1]) != command: return True
 dep=Path(str(obj)+'.d')
 if not dep.exists(): return True
 content=dep.read_text().replace('\\\n',' ')
 if ':' not in content: return True
 dependencies=shlex.split(content.split(':',1)[1]);stamp=obj.stat().st_mtime
 for name in dependencies:
  dependency=Path(name)
  if not dependency.is_absolute(): dependency=repo/'buildDir'/dependency
  if not dependency.exists() or dependency.stat().st_mtime>stamp: return True
 return False
with (dest/'build-tests.log').open('w') as log:
 for unit in units:
  obj=dest/(unit.replace('/','-')+'.o');cmd=base[:]
  for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
  cmd[-1]=str(repo/'tests/src/storage/relocatable'/(unit+'.cpp'))
  cmd=cmd[:]
  if needs_compile(obj,cmd):
   subprocess.run(cmd,cwd=repo/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
  objects.append(str(obj));commands.append(cmd)
(dest/'compile.json').write_text(json.dumps({'units':units,'commands':commands,'cwd':str(repo/'buildDir'),'full_suite':False},indent=2)+'\n')
args=json.loads((repo/'benchmarks/architecture-design/results/20261020-work-resume/range-default-final-link.json').read_text());args=[a for a in args if not a.endswith('.o')];idx=args.index('-o');args[idx:idx]=objects;args[args.index('-o')+1]=str(dest/'module-tests')
args=['/workspace/zyx-build-core-candidate/libzyx_core.a' if a=='libzyx_core.a' else '/workspace/zyx-build-core-candidate/libzyx_inputxx.a' if a=='libzyx_inputxx.a' else '/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libxxhash.so.0.8.3' if a=='-lxxhash' else '--dependency-file='+str(dest/'link.d') if a.startswith('--dependency-file=') else a for a in args]
(dest/'link.json').write_text(json.dumps(args,indent=2)+'\n')
print('module objects ready',flush=True)
