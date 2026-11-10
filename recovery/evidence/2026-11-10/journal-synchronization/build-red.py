from pathlib import Path
import json,shlex,subprocess
repo=Path('/workspace/zyx-ci-reconstructed');build=repo/'buildDir';out=Path('/workspace/zyx-journal-race-evidence')
source=repo/'tests/src/storage/relocatable/backend/test_RelocatableStorageBackendSchema_Maintenance.cpp'
s=source.read_text();needle='ASSERT_TRUE(waitUntil([&] { return backend.maintenanceMetrics().loop.deferred_attempts >= 1U; }));\n\t\tbackend.pauseMaintenance();';assert s.count(needle)==1
s=s.replace(needle,needle.replace('\n\t\tbackend.pauseMaintenance();','\n        std::this_thread::sleep_for(std::chrono::milliseconds(2000));\n\t\tbackend.pauseMaintenance();'))
f=out/'maintenance-late-observer.cpp';f.write_text(s)
c=next(x for x in json.load(open(build/'compile_commands.json')) if x['file']==str(source));a=shlex.split(c['command']);a[a.index('-o')+1]=str(out/'late.o');a[-1]=str(f)
with (out/'compile.log').open('w') as log:r=subprocess.run(a,cwd=build,stdout=log,stderr=subprocess.STDOUT)
assert not r.returncode
link=subprocess.check_output(['ninja','-C',str(build),'-t','commands','zyx_test_suite'],text=True).splitlines()[-1];a=shlex.split(link.removeprefix(': && ').removesuffix(' && :'));old='CMakeFiles/zyx_test_suite.dir/tests/src/storage/relocatable/backend/test_RelocatableStorageBackendSchema_Maintenance.cpp.o';assert old in a;a[a.index(old)]=str(out/'late.o');a[a.index('-o')+1]=str(out/'late-observer');
with (out/'link.log').open('w') as log:r=subprocess.run(a,cwd=build,stdout=log,stderr=subprocess.STDOUT)
assert not r.returncode
r=subprocess.run([str(out/'late-observer'),'--gtest_filter=RelocatableStorageBackendSchemaTest.LargeValueMaintenanceJournalResumesAcrossReopenAtEveryDurablePhase','--gtest_output=xml:'+str(out/'late.xml')],cwd=repo,stdout=(out/'late.log').open('w'),stderr=subprocess.STDOUT);print('controlled late observer exit',r.returncode,flush=True)
