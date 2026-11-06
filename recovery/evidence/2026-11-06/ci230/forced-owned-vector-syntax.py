import pathlib,subprocess,shlex,json
r=pathlib.Path('/workspace/zyx-ci-fix');b=pathlib.Path('/workspace/zyx-ci-config');o=pathlib.Path('/tmp/zyx-ci230-force-owned');h=o/'include/graph/utils/OwnedVector.hpp';h.parent.mkdir(parents=True,exist_ok=True);h.write_text((r/'include/graph/utils/OwnedVector.hpp').read_text().replace('#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0','#if 1'))
files=subprocess.check_output(['git','diff','--name-only'],cwd=r,text=True).splitlines();paths=['src/query/execution/LogicalPlanProcedureRuntime.cpp', 'src/query/gds/LogicalGraphAlgorithms.cpp', 'src/storage/relocatable/entity/NativeGraphWriteCommit.cpp', 'src/storage/relocatable/entity/NativeGraphWritePlanner.cpp', 'src/storage/relocatable/metadata/MetadataPagePool.cpp', 'src/storage/relocatable/metadata/MetadataWriteBatch.cpp', 'src/storage/relocatable/index/BTreePage.cpp', 'src/storage/relocatable/allocation/SlottedPage.cpp', 'src/storage/relocatable/backend/RelocatableStorageBackend.cpp']
log=[]
for p in dict.fromkeys(paths):
 t='CMakeFiles/zyx_core.dir/'+p+'.o';a=shlex.split(subprocess.check_output(['ninja','-t','commands',t],cwd=b,text=True).splitlines()[-1]);c=[a[0],'-I'+str(o/'include')];i=1
 while i<len(a):
  if a[i] in ('-MT','-MF','-o'):i+=2;continue
  if a[i] in ('-MD','-c','-fprofile-instr-generate','-fcoverage-mapping'):i+=1;continue
  c.append(a[i]);i+=1
 c+=['-fsyntax-only'];f=o/(pathlib.Path(p).stem+'-syntax.log')
 with f.open('w') as out:x=subprocess.run(c,cwd=b,stdout=out,stderr=subprocess.STDOUT)
 print(p,x.returncode,flush=True);log.append({'file':p,'exit':x.returncode,'command':c})
 if x.returncode:break
(o/'syntax-results.json').write_text(json.dumps(log,indent=2)+'\n')
raise SystemExit(any(x['exit'] for x in log))
