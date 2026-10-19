from pathlib import Path
import subprocess,shlex,json,sys
root=Path(__file__).resolve().parents[4];b=Path(sys.argv[1]) if len(sys.argv)>1 else root/'buildDir';r=Path(sys.argv[2]) if len(sys.argv)>2 else root/'benchmarks/architecture-design/results/20261019-r2-reproduction';r.mkdir(parents=True,exist_ok=True)
paths=[root/p for p in [
 'tests/src/storage/relocatable/allocation/test_SharedPagePoolAllocator.cpp',
 'tests/src/storage/relocatable/allocation/test_SharedPagePoolAllocatorFaults.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableBoundExecutionSemantics.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp',
 'tests/src/query/execution/test_LogicalPlanBoundedTopK.cpp',
 'tests/src/storage/relocatable/metadata/test_MetadataPageReadContext.cpp',
 'tests/src/storage/relocatable/metadata/test_LocationDirectory.cpp',
 'tests/src/storage/relocatable/property/test_PropertyCollectionRootStoreCache.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableDirectPropertyBatch.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableBoundProgramExecution.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableLogicalGraphReadSnapshot.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableQueryResources.cpp']]

commands=subprocess.check_output(['ninja','-C',str(b),'-t','commands','zyx_test_suite'],text=True).splitlines();objects=[]
for p in sorted(paths):
 line=next(v for v in commands if ' -c '+str(p) in v);args=shlex.split(line);objects.append(args[args.index('-o')+1])
manifest={'scope':'R1.3/R2 affected consumer, metadata, property and native resource modules; explicit filter, not full unit suite','modules':[str(p.relative_to(root)) for p in sorted(paths)],'module_count':len(paths),'scope_ids':['R1.3','R2.1','R2.2','R2.3']}
(r/'r2-five-test-registration.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Building',len(paths),'necessary modules',flush=True)
subprocess.run(['ninja','-C',str(b),'-j','4','libzyx_core.a',*objects],check=True)
link=shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'));link=[v for v in link if not v.endswith('.cpp.o')];index=link.index('-o');binary=r/'r2-five-tests';link[index:index]=objects;link[link.index('-o')+1]=str(binary);link.append('/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a');subprocess.run(link,cwd=b,check=True);(r/'r2-five-test-link.json').write_text(json.dumps(link,indent=2)+'\n')
subprocess.run([str(binary),'--gtest_filter=RelocatableSharedPagePoolAllocatorTest.*:RelocatableSharedPagePoolAllocatorFaultTest.*:LogicalPlanBoundedTopKTest.*:RelocatableStorageBackendTest.BoundSemantics*:RelocatableStorageBackendTest.BoundPaths*:RelocatableStorageBackendTest.BoundBlocking*:RelocatableStorageBackendTest.BoundImportedEntity*:RelocatableStorageBackendTest.NativeLogicalReadonlySortSpills*:RelocatableStorageBackendTest.NativeLogicalDistinctSpills*:RelocatableStorageBackendTest.NativeUnionDistinctWithScratch*:MetadataPageReadContextTest.*:RelocatableLocationDirectoryTest.*:PropertyCollectionRootStoreCacheTest.*:RelocatableStorageBackendTest.BoundNativeInputs*:RelocatableStorageBackendTest.BoundRemainingConsumers*:RelocatableStorageBackendTest.BoundMutationReadSides*:RelocatableStorageBackendTest.BoundCsv*:RelocatableStorageBackendTest.DirectPropertyCells*:RelocatableStorageBackendTest.NativeDirectFilter*:RelocatableStorageBackendTest.AdjacencyTopologyPagesRemain*:RelocatableStorageBackendTest.NativeColumnsGather*:RelocatableStorageBackendTest.CoordinatedSortJoin*:RelocatableStorageBackendTest.NativePinnedSnapshotFilePressure*','--gtest_output=json:'+str(r/'r2-five-final.json')],cwd=r,check=True)
