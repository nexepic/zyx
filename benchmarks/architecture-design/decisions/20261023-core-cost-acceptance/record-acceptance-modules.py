from pathlib import Path
import json,hashlib,shutil
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance';sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest();checks={}
for name,folder,stem,total,build in [('public','cold-public-conversion-hybrid','public-final-',12,'zyx-build-core-hybrid'),('hybrid-legacy','cold-row-ownership-hybrid','acceptance-legacy-',576,'zyx-build-core-hybrid'),('hybrid-compact','cold-row-ownership-hybrid','acceptance-compact-',576,'zyx-build-core-hybrid'),('default','cold-row-ownership-default','',155,'zyx-build-core-candidate'),('oneframe','closeout-oneframe-default','',2,'zyx-build-core-candidate')]:
 src=raw/folder;j=json.loads((src/(stem+'tests.json')).read_text());assert j['tests']==total and j['failures']==0 and j.get('skipped',0)==0
 receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['returncode']==0 and receipt['library_sha256']==sha(Path('/workspace')/build/'libzyx_core.a')
 checks[name]={'tests':total,'failures':0,'skipped':0,'library_sha256':receipt['library_sha256']}
 for n in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+n),d/(name+'-'+n))
 for n in ['compile.json','link.json']:shutil.copy2(src/n,d/(name+'-'+n))
for n in ['tests.json','tests.log','receipt.json']:shutil.copy2(raw/'cold-public-conversion-hybrid'/('public-initial-'+n),d/('initial-read-account-'+n))
for n in ['add-public-cold-acceptance-tests.py','add-public-cold-gc-relocation.py','update-final-acceptance-plan.py','record-acceptance-modules.py','prepare-acceptance-campaign.py','build-cold-public-conversion-hybrid.py','run-cold-public-conversion-hybrid.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py','build-closeout-oneframe-default.py','run-closeout-oneframe-default.py']:
 shutil.copy2(Path('/tmp')/n,d/n)
shutil.copy2(r/'benchmarks/architecture-design/decisions/20261023-core-cost-closeout/environment.json',d/'environment.json')
case=json.loads((d/'public-tests.json').read_text())['testsuites'][0]['testsuite'][0]
row=int(case['row_steady_bytes']);candidate=int(case['candidate_steady_bytes']);assert candidate*10<=row*9
(d/'module-checks.json').write_text(json.dumps({'full_suite':False,'progress_percent':90,'implementation_checks_passed':True,'performance_matrix_pending':True,'checks':checks,'whole_database_space':{'row_steady_bytes':row,'candidate_steady_bytes':candidate,'saving_fraction':1-candidate/row,'gate_fraction':0.10,'scope':'512 real ordinary ROW owners, five fields including long id and cold text; all8 groups prepared privately and switched together; this is not an allocation claim for medium/holdout','peak_limit_bytes':32*1024*1024,'conversion_budget_bytes':1024*1024},'test_boundary':'Public12 cases use real ROW sources, no descriptor byte injection. Other27 fixture lifecycle cases remain supplemental. Default/experimental variants are reported separately. Initial public read-account failure occurred after successful conversion/admission; caller read scope was fixed, not storage validation.'},indent=2)+'\n')
print(checks)
