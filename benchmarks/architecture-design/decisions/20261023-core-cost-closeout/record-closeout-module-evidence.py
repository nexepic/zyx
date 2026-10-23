from pathlib import Path
import hashlib,json,shutil,subprocess,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-closeout';sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
checks={}
for name,folder,stem,total,lib in [('hybrid-legacy','cold-row-ownership-hybrid','closeout-legacy-',564,'zyx-build-core-hybrid'),('hybrid-compact','cold-row-ownership-hybrid','closeout-compact-',564,'zyx-build-core-hybrid'),('default','cold-row-ownership-default','',154,'zyx-build-core-candidate'),('oneframe','closeout-oneframe-default','',2,'zyx-build-core-candidate')]:
 src=raw/folder;j=json.loads((src/(stem+'tests.json')).read_text());assert j['tests']==total and j['failures']==0 and j.get('skipped',0)==0
 receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['returncode']==0 and receipt['library_sha256']==sha(Path('/workspace')/lib/'libzyx_core.a')
 checks[name]={'tests':total,'failures':0,'skipped':0,'library_sha256':receipt['library_sha256']}
 for n in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+n),d/(name+'-'+n))
 for n in ['compile.json','link.json']:shutil.copy2(src/n,d/(name+'-'+n))
for n in ['before-tests.json','before-tests.log','before-receipt.json']:shutil.copy2(raw/'closeout-oneframe-default'/n,d/('oneframe-'+n))
for n in ['closeout-build-artifact-cleanup.json','closeout-obsolete-test-build-cleanup.json']:shutil.copy2(raw/n,d/n)
for n in ['implement-cold-multigroup.py','implement-cold-multigroup-coordinator.py','add-cold-multigroup-tests.py','add-cold-multigroup-guards.py','add-cold-multigroup-topology.py','record-closeout-module-evidence.py','prepare-closeout-campaign.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py','build-closeout-oneframe-default.py','run-closeout-oneframe-default.py']:
 shutil.copy2(Path('/tmp')/n,d/n)
shutil.copy2('/tmp/closeout-oneframe-throw-trace.cpp',d/'diagnostic-oneframe-throw-trace.cpp.txt');shutil.copy2('/tmp/closeout-oneframe-trace.log',d/'oneframe-before-throw-trace.log')
(d/'module-checks.json').write_text(json.dumps({'full_suite':False,'progress_percent':90,'P7_accepted':False,'commit_push':False,'checks':checks,'cross_group':'All27 actual lifecycle cases are included in each564-case experimental module report. Covers both kinds sharing a FieldPage; hot/cold/demotion, logical API, birth/delete, old pins and whole64-owner refusal. No forward conversion/adoption claim.','oneframe':'Actual full filter query failed at4096-byte cache before fix; now both4096 and64MiB pass, including typed source/read-after-write/old pin checks. Sequential one-frame reads use the original bounded owned fallback, without changing capacity or validation.'},indent=2)+'\n')
print(checks)
