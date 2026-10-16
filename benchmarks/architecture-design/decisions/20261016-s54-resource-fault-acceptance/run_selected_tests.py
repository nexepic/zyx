import pathlib, subprocess, shlex, json, hashlib, xml.etree.ElementTree as ET
root=pathlib.Path(__file__).resolve().parents[4]
sources=[
 'tests/src/storage/relocatable/backend/test_RelocatableQueryScratch.cpp',
 'tests/src/storage/relocatable/backend/test_RelocatableQueryResources.cpp',
 'tests/src/query/execution/test_ExternalQuerySort.cpp',
 'tests/src/query/execution/test_QuerySpillRowCodec.cpp',
 'tests/src/query/execution/test_LogicalPlanDistinctAllocator.cpp',
 'tests/src/query/execution/test_LogicalPlanSortGrowth.cpp',
 'tests/src/query/execution/test_ResourceCoordinator.cpp',
 'tests/src/query/execution/test_ProcessResourceDomains.cpp',
 'tests/src/query/execution/test_PrepaidMemoryResource.cpp',
 'tests/src/query/execution/test_BudgetMemoryResource.cpp',
 'tests/src/storage/relocatable/maintenance/test_RelocatableMaintenanceService.cpp',
 'tests/src/query/api/test_QueryResultResources.cpp',
]
objects=['CMakeFiles/zyx_test_suite.dir/'+p+'.o' for p in sources]
subprocess.run(['cmake','--build','buildDir','--target','zyx_core',*objects,'-j','4'],cwd=root,check=True)
lines=(root/'buildDir/build.ninja').read_text().splitlines()
start=next(i for i,v in enumerate(lines) if v.startswith('build zyx_test_suite:'))
libs=next(v.split(' = ',1)[1] for v in lines[start:start+14] if v.strip().startswith('LINK_LIBRARIES = '))
command=['/workspace/zyx-sysroot/usr/bin/clang++-19','-fprofile-instr-generate','-fcoverage-mapping','-pthread',*[str(root/'buildDir'/o) for o in objects],'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a',*shlex.split(libs),'-o','/tmp/zyx-s54-acceptance-tests']
subprocess.run(command,cwd=root/'buildDir',check=True)
subprocess.run(['/tmp/zyx-s54-acceptance-tests','--gtest_output=xml:/tmp/zyx-s54-acceptance-tests.xml'],cwd=root,check=True)
x=ET.parse('/tmp/zyx-s54-acceptance-tests.xml').getroot()
report={'tests':int(x.attrib['tests']),'failures':int(x.attrib['failures']),'disabled':int(x.attrib.get('disabled',0)),'suites':len(list(x)),'sources':sources,'full_suite':False,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()}
pathlib.Path('/tmp/zyx-s54-acceptance-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
