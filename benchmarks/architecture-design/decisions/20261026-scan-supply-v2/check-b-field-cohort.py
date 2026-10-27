"""Untimed B real-medium layout and complete public-result validation."""
from pathlib import Path
import hashlib,json,shutil,sys
from process_receipt import run
D=Path(__file__).resolve().parent;R=D.parents[3];B=D/'b-field-cohort';O=B/'medium-validation'
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def read(p):return json.loads(p.read_text())
def save(p,v):p.write_text(json.dumps(v,indent=2)+'\n')
conversion=read(B/'medium-conversion-receipt.json')
assert conversion['resources']['returncode']==0
assert read(B/'generality-lifecycle-receipt.json')['returncode']==0
assert not O.exists();O.mkdir()
source=B/'medium.zyx';before=sha(source)
probe_receipt=read(B/'probes/receipt.json')
assert sha(R/'build-scan-candidate/libzyx_core.a')==probe_receipt['library_sha256']
for name,digest in probe_receipt['binaries'].items():assert sha(B/'probes'/name)==digest
assert conversion['target_sha256']==before
for relative,digest in read(B/'source-freeze.json')['engine_source_hashes'].items():assert sha(R/relative)==digest
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
cmd=[str(B/'probes/values'),'check-read',str(source),str(O/'work.db'),','.join(cases),'0','1']
with (O/'work.jsonl').open('w') as out,(O/'work.stderr').open('w') as err:
 resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'],timeout=1200)
save(O/'work-resources.json',resources)
assert resources['returncode']==0
rows=[json.loads(x) for x in (O/'work.jsonl').read_text().splitlines() if x.startswith('{')]
assert len(rows)==4
for case,event in zip(cases,rows):campaign.check_event(case,event,'medium',False)
private=O/'census-private.db';shutil.copyfile(source,private)
cmd=[str(B/'probes/census'),str(private)]
with (O/'census.json').open('w') as out,(O/'census.stderr').open('w') as err:
 resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'],timeout=1200)
save(O/'census-resources.json',resources)
assert resources['returncode']==0
c=read(O/'census.json');original=read(D/'layout-census/result.json')
for key in ['examined','groups','typed_score_groups','encoded_score_bytes','nonnull_primitive_bytes']:
 assert c[key]==original[key],(key,c[key],original[key])
gates=read(D/'b-field-cohort-registration.json')['work_gate']
passed=(c['score_pages']<=original['score_pages']*gates['score_pages_max_fraction_of_A'] and c['union_pages']<=original['union_pages']*gates['score_and_group_page_union_max_fraction_of_A'])
assert sha(source)==before
save(O/'receipt.json',dict(work_gate_passed=passed,complete_public_oracles=4,timing_benchmark=False,source_sha256=before,probe_receipt_sha256=sha(B/'probes/receipt.json'),score_page_ratio=c['score_pages']/original['score_pages'],union_page_ratio=c['union_pages']/original['union_pages'],progress_percent=45))
assert passed,'Preserve failure; do not tune against holdout or launch formal timing'
print('PASS B medium complete oracles and preregistered exact-layout gates',flush=True)
