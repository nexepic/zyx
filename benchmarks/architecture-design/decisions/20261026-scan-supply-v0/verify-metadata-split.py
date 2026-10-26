"""Build only the affected metadata module with the StorageIO fault seam.

The production archive is unchanged. StorageIO's hook-only nested declarations
add no StorageIO data members; link its complete test-instrumented implementation
before the archive so fault callbacks are actually exercised.
"""
from pathlib import Path
import json,subprocess,hashlib
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent
recipe=json.loads((R/'benchmarks/architecture-design/decisions/20261026-s4-native-write-cost/default-recipes.json').read_text());objects=[];commands=[]
sources=['src/storage/StorageIO.cpp','tests/src/storage/relocatable/metadata/test_RelocatableMetadataStore.cpp','tests/src/storage/relocatable/metadata/test_RelocatableMetadataStoreReservations.cpp']
with (D/'metadata-hook-build.log').open('w') as log:
 for source in sources:
  c=recipe['compile'][0].copy();obj=Path('/tmp')/(Path(source).stem+'-v0-hooks.o');c[c.index('-c')+1]=str(R/source);c[c.index('-o')+1]=str(obj);c.insert(1,'-DZYX_ENABLE_STORAGE_TEST_HOOKS=1');commands.append(c);subprocess.run(c,stdout=log,stderr=subprocess.STDOUT,check=True);objects.append(str(obj))
 c=recipe['link'].copy();c[1:2]=objects;c[c.index('-o')+1]='/tmp/zyx-v0-metadata-hook-tests';commands.append(c);subprocess.run(c,stdout=log,stderr=subprocess.STDOUT,check=True)
with (D/'metadata-hook-tests.log').open('w') as log:subprocess.run(['/tmp/zyx-v0-metadata-hook-tests','--gtest_output=json:'+str(D/'metadata-hook-tests.json')],stdout=log,stderr=subprocess.STDOUT,check=True)
result=json.loads((D/'metadata-hook-tests.json').read_text());assert result['failures']==0
(D/'metadata-hook-receipt.json').write_text(json.dumps({'commands':commands,'test_count':result['tests'],'failures':result['failures'],'scope':'affected metadata module only; StorageIO hook overlay, production archive unchanged'},indent=2)+'\n');print(result['tests'],'metadata tests passed',flush=True)
