from pathlib import Path
import hashlib,json,subprocess
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/results/20261024-work-scan';out=d/'final-comparison';out.mkdir(exist_ok=True)
assert not (out/'freeze.json').exists(),'final comparison is already frozen; never overwrite campaign identity'
artifacts={}
versions={'medium':'v12','holdout':'v11'}
paths={'initial_binary':d/'public-baseline','control_binary':Path('/tmp/zyx-frozen-control-public'),'A_binary':Path('/tmp/zyx-final-a-public'),'B_binary':Path('/tmp/zyx-final-b-public'),'A_library':r/'build-scan-row-production/libzyx_core.a','B_library':r/'build-scan-candidate/libzyx_core.a','A_values':Path('/tmp/zyx-final-a-values'),'B_values':Path('/tmp/zyx-final-b-values'),'campaign':r/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison/campaign.py'}
for scale in ['medium','holdout']:
 for kind,suffix in [('row','.zyx'),('scan','.scan-private-'+versions[scale]+'.zyx'),('kuzu','.kuzu')]:paths[f'{scale}_{kind}']=d/(scale+suffix)
paths['large_guard']=d/'guard-frozen-large.zyx';paths['skew_guard']=d/'guard-skew-indexed.zyx'
paths['control_library']=Path('/workspace/zyx-frozen-control-369039d/build/libzyx_core.a')
paths['initial_library']=d/'libzyx-baseline-production.a'
paths['freeze_script']=r/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison/freeze.py'
paths['evaluation_script']=r/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison/evaluate.py'
paths['construction_cost_script']=r/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison/construction-costs.py'
paths['values_probe_source']=r/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison/value-probe.cpp'
paths['public_probe_source']=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/public-probe.cpp'
paths['oracle_source']=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/oracle.py'
paths['kuzu_worker_source']=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/kuzu-worker.py'
for scale in ['medium','holdout']:
 for csv_path in (d/scale).glob('*.csv'):paths[f'{scale}_{csv_path.name}']=csv_path
 paths[f'{scale}_manifest']=d/scale/'manifest.json'

for name,p in paths.items():artifacts[name]={'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest()}
# Never freeze an image without successful final full integrity and actual space admission.
for scale in ['medium','holdout']:
 log=Path(f'/tmp/zyx-{scale}-scan-construction-{versions[scale]}.log').read_text()
 assert '"generation":' in log and 'construction_error=' not in log,(scale,'incomplete')
 assert artifacts[f'{scale}_scan']['bytes']*10<=artifacts[f'{scale}_row']['bytes']*9
source_files={}
for tree in ['include','src','apps/scan_convert','cmake']:
 for path in (r/tree).rglob('*'):
  if path.is_file() and path.suffix in ['.cpp','.hpp','.h','.cmake']:
   source_files[str(path.relative_to(r))]=hashlib.file_digest(path.open('rb'),'sha256').hexdigest()
source_files['CMakeLists.txt']=hashlib.file_digest((r/'CMakeLists.txt').open('rb'),'sha256').hexdigest()
(out/'source-files.json').write_text(json.dumps(source_files,indent=2)+'\n')
kuzu_version=subprocess.check_output(['/workspace/zyx-tools/bin/python','-c','import kuzu;print(kuzu.__version__)'],text=True).strip()
assert kuzu_version=='0.11.3',kuzu_version
(out/'freeze.json').write_text(json.dumps({'kuzu_version':kuzu_version,'construction_versions':versions,'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'artifacts':artifacts,'query_measurement_started':False,'protocol':{'processes':3,'warmup':3,'measured':10,'threads':1,'complete_oracles':True,'historical_AMD_times_reused':False,'extra_cache_budget':0,'legacy_guard_policy':'Explicit isolated B conversion refused for guards; A on unchanged legacy ROW image is the non-adopted path; no B native adoption claim'}},indent=2)+'\n')
