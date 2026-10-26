from pathlib import Path
import json,subprocess,sys
D=Path(__file__).resolve().parent;R=D.parents[3]
hybrid='--hybrid' in sys.argv
O=D/'hybrid' if hybrid else D
O.mkdir(exist_ok=True)
base=json.loads((D/'base-recipe.json').read_text())
sources=[
 'src/storage/StorageIO.cpp',
 'tests/src/query/execution/test_DirectPrimitiveColumnFilter.cpp',
 'tests/src/storage/relocatable/metadata/test_RelocatableMetadataStore.cpp',
 'tests/src/storage/relocatable/metadata/test_RelocatableMetadataStoreReservations.cpp',
 'tests/src/query/execution/test_BoundNodeColumnPredicates.cpp',
 'tests/src/storage/relocatable/property/test_HybridPropertyInlineMutation.cpp',
 'tests/src/storage/relocatable/property/test_HybridPropertyColumnReadReuse.cpp',
 'tests/src/storage/relocatable/property/test_HybridPropertyPublicLifecycle.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableDirectPropertyBatch.cpp']
commands=[];objects=[]
for source in sources:
 args=base['compile'][0].copy()
 if hybrid: args=[x.replace('-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=1','-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=0').replace('/build-scan-candidate','/build-hybrid-compat') for x in args]
 args += ['-DZYX_ENABLE_STORAGE_TEST_HOOKS=1','-I'+str(R/'src/query/parser/cypher'), '-I/workspace/antlr4-install/include/antlr4-runtime'];args[args.index('-c')+1]=str(R/source)
 obj=str(O/(Path(source).stem+'.o'));args[args.index('-o')+1]=obj
 objects.append(obj);commands.append(args)
link=[x for x in base['link'] if not x.endswith('.o')]
if hybrid: link=[x.replace('/build-scan-candidate','/build-hybrid-compat') for x in link]
link[1:1]=objects;link[link.index('-o')+1]=str(O/'modules');commands.append(link)
(O/'module-recipes.json').write_text(json.dumps(commands,indent=2)+'\n')
with (O/'module-build.log').open('w') as log:
 for command in commands:
  if '-c' in command and Path(command[command.index('-o')+1]).exists() and 'resume' in sys.argv: continue
  print('BUILD',command[command.index('-c')+1] if '-c' in command else 'link',flush=True)
  subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,cwd=R)
