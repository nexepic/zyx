"""One necessary B shared-pool reuse regression after all timed workers exit."""
from pathlib import Path
import hashlib,json,os,subprocess,time,tarfile
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;B=D/'b-field-cohort';O=D/'b-shared-pool-reuse';pipeline=51300
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
command=subprocess.check_output(['ps','-p',str(pipeline),'-o','args='],text=True).strip()
assert command.endswith('run-b-final-pipeline.py')
print('Waiting for timed guards and untimed write-cost campaign to exit; no compiler running',flush=True)
deadline=time.monotonic()+21600
while True:
 try:state=subprocess.check_output(['ps','-p',str(pipeline),'-o','stat='],text=True).strip()
 except subprocess.CalledProcessError:break
 if state.startswith('Z'):break
 if time.monotonic()>deadline:raise TimeoutError('Final campaign did not exit')
 time.sleep(1)
assert (B/'final-guards/summary.json').exists() and (B/'write-costs/summary.json').exists(),'Preserve campaign failure; do not overlap tests'
freeze=json.loads((B/'source-freeze.json').read_text());assert sha(R/'build-scan-candidate/libzyx_core.a')==freeze['library_sha256']
source=R/'tests/src/storage/relocatable/entity/test_BuildingScanFieldCohorts.cpp';text=source.read_text();marker='TEST_F(BuildingScanFieldCohortTest, SharedPoolRetirementWaitsForOldPinThenReusesItsPhysicalSlot) {'
start=text.index(marker);end=text.index('\n}\n',start)+3
previous=text[:start]+text[end:]
with tarfile.open(B/'frozen-source.tar.gz','r:gz') as archive:
 archived=archive.extractfile(str(source.relative_to(R))).read().decode()
assert previous==archived,'Existing nine cases/fixture changed; expand only affected module checks if needed'
assert not O.exists();O.mkdir();P=D/'b-heterogeneous-spill'
for name in ['compile','link']:
 cmd=json.loads((P/(name+'-command.json')).read_text());cmd=[x.replace(str(P),str(O)) for x in cmd]
 (O/(name+'-command.json')).write_text(json.dumps(cmd,indent=2)+'\n')
 with (O/(name+'.log')).open('w') as output:result=subprocess.run(cmd,stdout=output,stderr=output)
 assert result.returncode==0,(name,result.returncode)
cmd=[str(O/'module'),'--gtest_filter=BuildingScanFieldCohortTest.SharedPoolRetirementWaitsForOldPinThenReusesItsPhysicalSlot','--gtest_output=json:'+str(O/'test.json')]
with (O/'test.log').open('w') as output:result=subprocess.run(cmd,stdout=output,stderr=output,timeout=180)
receipt=dict(returncode=result.returncode,command=cmd,source_sha256=sha(source),library_sha256=freeze['library_sha256'],existing_nine_cases_and_fixture_unchanged=True,scope='One new post-window retirement/pin-release/physical-slot reuse case only; no full suite or performance rerun',performance_source_archive='b-field-cohort/frozen-source.tar.gz')
(O/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');assert result.returncode==0
print('PASS shared pool pin protection, release, exact physical-slot reuse, complete old/current values, integrity and reopen',flush=True)
