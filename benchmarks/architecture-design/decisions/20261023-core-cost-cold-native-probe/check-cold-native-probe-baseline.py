from pathlib import Path
import subprocess,json,hashlib,re,shlex,os
r=Path('/workspace/zyx');out=r/'benchmarks/architecture-design/results/20261022-core-cost/native-probe-baseline';out.mkdir(exist_ok=True)
frozen=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-conversion-preparation';patch=(frozen/'source.patch').read_text();snapshot=out/'source';snapshot.mkdir(exist_ok=True)
paths=['include/graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp','tests/src/storage/relocatable/property/test_HybridPropertyOrdinaryConversion.cpp']
def reconstruct(path):
 original=subprocess.check_output(['git','show','HEAD:'+path],cwd=r,text=True).splitlines(keepends=True)
 section=patch.split('diff --git a/'+path+' b/'+path+'\n',1)
 if len(section)==1:return ''.join(original)
 lines=section[1].split('diff --git ',1)[0].splitlines(keepends=True);result=[];position=0;i=0
 while i<len(lines):
  match=re.match(r'@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@',lines[i])
  if not match:i+=1;continue
  start=int(match[1])-1;result.extend(original[position:start]);position=start;i+=1
  while i<len(lines) and not lines[i].startswith('@@ '):
   line=lines[i]
   if line.startswith(' '):assert original[position]==line[1:];result.append(original[position]);position+=1
   elif line.startswith('-'):assert original[position]==line[1:];position+=1
   elif line.startswith('+'):result.append(line[1:])
   elif line.startswith('\\'):pass
   else:break
   i+=1
 result.extend(original[position:]);return ''.join(result)
for path in paths:
 p=snapshot/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(reconstruct(path))
assert (snapshot/paths[1]).read_bytes()==(r/paths[1]).read_bytes(), 'baseline test source changed since frozen round'
meta=(r/paths[0]).read_text();new='''        // Same Native coordinator's explicit preparation-only cold layout.
        // Publication remains closed until the complete mode3 lifecycle passes.
        bool cold_row_remainder = false;
''';assert new in meta;assert meta.replace(new,'',1)==(snapshot/paths[0]).read_text(), 'another header change needs baseline reconstruction'
current=r/'benchmarks/architecture-design/results/20261022-core-cost/cold-row-ownership-hybrid';recipe=json.loads((current/'compile.json').read_text());template=next(cmd for cmd in recipe['commands'] if cmd[-1].endswith('/test_HybridPropertyOrdinaryConversion.cpp'));cmd=template[:];obj=out/'ordinary-baseline.o';cmd.insert(1,'-I'+str(snapshot/'include'))
for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(obj)+('.d' if flag=='-MF' else '')
cmd[-1]=str(snapshot/paths[1])
with (out/'compile.log').open('w') as log:subprocess.run(cmd,cwd=r/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
hook=shlex.split(Path('/tmp/short-text-storageio-compile.txt').read_text());hook.insert(1,'-DZYX_ENABLE_STORAGE_TEST_HOOKS=1');hook.insert(1,'-I'+str(snapshot/'include'));hookobj=out/'storageio-baseline.o'
for flag in ['-o','-MT','-MF']:hook[hook.index(flag)+1]=str(hookobj)+('.d' if flag=='-MF' else '')
with (out/'hook.log').open('w') as log:subprocess.run(hook,cwd='/workspace/zyx-build-core-hybrid',stdout=log,stderr=subprocess.STDOUT,check=True)
lib=Path('/workspace/zyx-cold-conversion-preparation-hybrid-candidate/libzyx_core.a');args=json.loads((current/'link.json').read_text());args=[a for a in args if not a.endswith('.o')];idx=args.index('/workspace/zyx-build-core-hybrid/libzyx_core.a');args[idx:idx]=[str(obj),str(hookobj)];args=[str(lib) if a=='/workspace/zyx-build-core-hybrid/libzyx_core.a' else '--dependency-file='+str(out/'link.d') if a.startswith('--dependency-file=') else a for a in args];args[args.index('-o')+1]=str(out/'baseline-tests')
with (out/'link.log').open('w') as log:subprocess.run(args,cwd=r/'buildDir',stdout=log,stderr=subprocess.STDOUT,check=True)
f='HybridPropertyOrdinaryConversionTest.CompleteBoundedDenseGroupPreparesAllDirectoryPlansWithinTheConversionAccount:HybridPropertyOrdinaryConversionTest.FieldMapPlanningQuotaAndBothCancellationBoundariesRefundForRetry'
with (out/'tests.log').open('w') as log:p=subprocess.run([str(out/'baseline-tests'),'--gtest_filter='+f,'--gtest_output=json:'+str(out/'tests.json')],env=dict(os.environ,ZYX_TEST_COMPACT_COLUMN_MEMBERSHIPS='1',LLVM_PROFILE_FILE='/tmp/cold-native-probe-baseline-%p.profraw'),stdout=log,stderr=subprocess.STDOUT)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest();report=json.loads((out/'tests.json').read_text());receipt={'tests':report['tests'],'failures':report['failures'],'returncode':p.returncode,'full_suite':False,'test_filter':f,'compact_memberships':True,'library':str(lib),'library_sha256':sha(lib),'matching_header':'Reconstructed exactly from frozen source.patch against unchanged HEAD; current header differs only by newly added cold selector, removed in baseline overlay.','head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'source_hashes':{path:sha(snapshot/path) for path in paths},'unchanged_test_source_verified':True,'compile':cmd,'hook_compile':hook,'link':args};(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print({'tests':report['tests'],'failures':report['failures'],'library_sha256':sha(lib)})
