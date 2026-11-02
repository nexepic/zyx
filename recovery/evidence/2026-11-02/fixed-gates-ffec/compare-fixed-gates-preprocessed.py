import shlex,subprocess,hashlib,pathlib
wd='/workspace/zyx-unified-build'; before='/workspace/zyx-latestonly/'; after='/workspace/zyx-unified-fixtures/'
cmds=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=wd,text=True).splitlines()
files=['src/storage/relocatable/backend/RelocatableLogicalGraphRead.cpp','src/storage/relocatable/backend/RelocatableStorageBackend.cpp','src/storage/relocatable/entity/ScanReadProof.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreScanMaintenance.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreScanMembership.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreScanMutation.cpp','tests/src/storage/relocatable/entity/test_BuildingScanMembership.cpp','tests/src/storage/relocatable/property/test_HybridPropertyQueryParity.cpp']
for rel in files:
 line=next(x for x in cmds if x.endswith(' -c '+before+rel));a=shlex.split(line)
 for flag in ['-MT','-MF','-o']:
  i=a.index(flag);del a[i:i+2]
 if '-MD' in a:a.remove('-MD')
 a.insert(1,'-ffile-prefix-map='+after+'='+before);a.insert(1,'-P');a.insert(1,'-E');a[-1]=before+rel
 x=subprocess.run(a,cwd=wd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
 a.insert(1,'-I'+after+'tests/include');a.insert(1,'-I'+after+'include');a.insert(1,'-I'+after+'src');a.insert(1,'-I'+after);a[-1]=after+rel
 y=subprocess.run(a,cwd=wd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
 print(rel,'MATCH' if x==y else 'DIFF',len(x),len(y),hashlib.sha256(x).hexdigest(),hashlib.sha256(y).hexdigest(),flush=True)
