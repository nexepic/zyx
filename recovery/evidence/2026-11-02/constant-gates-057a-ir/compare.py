import shlex,subprocess,pathlib,hashlib,re,sys
wd='/workspace/zyx-unified-build';before='/workspace/zyx-latestonly/';after='/workspace/zyx-unified-fixtures/'
cmds=subprocess.check_output(['/workspace/zyx-tools/bin/ninja','-t','commands','zyx_test_suite'],cwd=wd,text=True).splitlines()
files=['src/storage/relocatable/format/Superblock.cpp','src/storage/relocatable/entity/NativeGraphWriteCommit.cpp','src/storage/relocatable/metadata/RelocatableMetadataStore.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreGraphMutation.cpp','src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp','src/storage/relocatable/metadata/MetadataStoreReclamation.cpp','src/storage/relocatable/property/PropertyCollectionRootStore.cpp']
for i,rel in enumerate(files,1):
 a=shlex.split(next(x for x in cmds if x.endswith(' -c '+before+rel)))
 for flag in ('-MT','-MF','-o'):
  n=a.index(flag);del a[n:n+2]
 if '-MD' in a:a.remove('-MD')
 a=[arg for arg in a if arg != '-g']
 a.remove('-c');a.insert(1,'-ffile-prefix-map='+after+'='+before);a.insert(1,'-g0');a.insert(1,'-O1');a.insert(1,'-emit-llvm');a.insert(1,'-S');a.insert(1,'-o');a.insert(2,'-');a[-1]=before+rel
 x=subprocess.run(a,cwd=wd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
 a.insert(1,'-I'+after+'tests/include');a.insert(1,'-I'+after+'include');a.insert(1,'-I'+after+'src');a.insert(1,'-I'+after);a[-1]=after+rel
 y=subprocess.run(a,cwd=wd,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True).stdout
 def norm(z):
  z=re.sub(rb"^; ModuleID = .*\n",b'',z,flags=re.M);z=re.sub(rb'^source_filename = .*\n',b'',z,flags=re.M);return z.replace(after.encode(),before.encode())
 x,y=norm(x),norm(y);print(f'{i}/{len(files)} {rel} '+('MATCH' if x==y else 'DIFF')+f' {len(x)} {len(y)}',flush=True)
 if x!=y:
  pathlib.Path('/workspace/zyx-link-tmp/ir-nodebug-before-'+pathlib.Path(rel).name+'.ll').write_bytes(x);pathlib.Path('/workspace/zyx-link-tmp/ir-nodebug-after-'+pathlib.Path(rel).name+'.ll').write_bytes(y)
