"""Targeted current metadata source overlay; unchanged dependencies from ROW archive."""
from pathlib import Path
import json,subprocess
D=Path(__file__).resolve().parent;R=D.parents[3];O=D/'metadata-row';O.mkdir(exist_ok=True)
a=json.loads((D/'base-recipe.json').read_text())
sources=['src/storage/StorageIO.cpp','src/storage/relocatable/metadata/RelocatableMetadataStore.cpp','src/storage/relocatable/metadata/MetadataStorePageMutation.cpp','src/storage/relocatable/metadata/MetadataStoreReclamation.cpp','src/storage/relocatable/metadata/MetadataStoreLookup.cpp','src/storage/relocatable/metadata/LocationDirectory.cpp','src/storage/relocatable/metadata/LocationDirectoryTypedAccess.cpp','src/storage/relocatable/index/BTreePageStore.cpp','tests/src/storage/relocatable/metadata/test_RelocatableMetadataStore.cpp','tests/src/storage/relocatable/metadata/test_RelocatableMetadataStoreReservations.cpp']
commands=[];objects=[]
for source in sources:
 c=[x.replace('-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=1','-DZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT=0').replace('-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=1','-DZYX_EXPERIMENTAL_SCAN_PATH_FORMAT=0').replace('/build-scan-candidate','/build-scan-row-production') for x in a['compile'][0]]
 c+=['-DZYX_ENABLE_STORAGE_TEST_HOOKS=1','-Wno-unused-function']
 obj=str(O/(Path(source).stem+'.o'));c[c.index('-c')+1]=str(R/source);c[c.index('-o')+1]=obj
 commands.append(c);objects.append(obj)
link=[x.replace('/build-scan-candidate','/build-scan-row-production') for x in a['link'] if not x.endswith('.o')]
link[1:1]=objects;link[link.index('-o')+1]=str(O/'modules');commands.append(link)
(O/'recipes.json').write_text(json.dumps(commands,indent=2)+'\n')
with (O/'build.log').open('w') as log:
 for c in commands:
  print('BUILD',c[c.index('-c')+1] if '-c' in c else 'link',flush=True)
  subprocess.run(c,stdout=log,stderr=subprocess.STDOUT,check=True,cwd=R)
