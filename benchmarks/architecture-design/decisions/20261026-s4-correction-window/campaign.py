"""One correction window: changed write families and repaired Skew input only."""
from pathlib import Path
import hashlib,json,subprocess,sys
HERE=Path(__file__).resolve().parent
REPO=Path('/workspace/zyx')
sys.path.insert(0,str(HERE.parent/'20261025-final-scan-comparison'))
import campaign as worker
BASE=worker.INPUT_ROOT
ROOT=BASE/'s4-correction-window';ROOT.mkdir(exist_ok=True)
TOOLS=BASE/'s4-native-write-cost'
def sha(p):
 with Path(p).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def artifacts():
 result={}
 paths={'B':TOOLS/'public-b-final','legacy':TOOLS/'public-legacy-final','initial':BASE/'public-baseline','control':BASE/'s3-lifecycle/public-control','skew':BASE/'guard-skew-complete-v1.zyx','B_library':REPO/'build-scan-candidate/libzyx_core.a','worker':HERE.parent/'20261025-final-scan-comparison/campaign.py','oracle':HERE.parent/'20261023-core-cost-acceptance/oracle.py','campaign':Path(__file__),'protocol':HERE/'protocol.json'}
 for scale in ['medium','holdout']:
  paths[scale+'_row']=BASE/(scale+'.zyx');paths[scale+'_scan']=BASE/(scale+'.scan-corrected-v1.zyx')
 for key,p in paths.items():result[key]={'path':str(p),'sha256':sha(p),'bytes':p.stat().st_size}
 return result
if sys.argv[1]=='freeze':
 assert not (ROOT/'freeze.json').exists()
 record={'artifacts':artifacts(),'source_sha256':{str(p.relative_to(REPO)):sha(p) for tree in ['src','include'] for p in (REPO/tree).rglob('*') if p.suffix in ['.cpp','.hpp','.h']},'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'scope':'Only two changed write families and repaired registered Skew fixture; never repeat the full matrix. Original final-delivery evidence remains immutable.'}
 (ROOT/'freeze.json').write_text(json.dumps(record,indent=2)+'\n');print('frozen');sys.exit(0)
frozen=json.loads((ROOT/'freeze.json').read_text())
for key,a in frozen['artifacts'].items():assert sha(a['path'])==a['sha256'],key
for p,h in frozen['source_sha256'].items():assert sha(REPO/p)==h,p
worker.ROOT=ROOT;worker.OUT=ROOT/'workers';worker.OUT.mkdir(exist_ok=True);worker.FREEZE_ID=sha(ROOT/'freeze.json')
inputs=ROOT/'inputs';inputs.mkdir(exist_ok=True)
for name in ['medium.zyx','holdout.zyx']:
 p=inputs/name
 if not p.exists():p.symlink_to(BASE/name)
p=inputs/'guard-skew-indexed.zyx'
if not p.exists():p.symlink_to(BASE/'guard-skew-complete-v1.zyx')
worker.INPUT_ROOT=inputs
(worker.ROOT/'r7-inputs').mkdir(exist_ok=True)
for scale in ['medium','holdout']:
 p=worker.ROOT/'r7-inputs'/scale
 if not p.exists():p.symlink_to(BASE/scale,target_is_directory=True)
worker.oracle.ROOT=ROOT
worker.BINARIES.update({k:frozen['artifacts'][k]['path'] for k in ['B','legacy','initial','control']})
worker.B_IMAGES={s:Path(frozen['artifacts'][s+'_scan']['path']) for s in ['medium','holdout']}
for round_number in range(3):
 for scale in ['medium','holdout']:
  for case in ['edge_batch_1000','edge_create_delete']:
   for engine in (['control','B'] if round_number%2==0 else ['B','control']):worker.worker(engine,scale,case,[case],round_number,'write')
 for engine in (['initial','control','legacy'] if round_number%2==0 else ['legacy','control','initial']):worker.worker(engine,'skew','repaired-skew',['skew_out','skew_both','skew_twohop'],round_number)
assert len(worker.results)==33
report={'freeze_sha256':worker.FREEZE_ID,'processes':len(worker.results),'passed':sum(r['status']=='passed' for r in worker.results),'samples':sum(len(r['samples']) for r in worker.results),'old_failed_raw_retained':True,'old_results_overwritten':False,'full_matrix_repeated':False,'results':worker.results}
(ROOT/'evaluation.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='results'}),flush=True)
assert report['passed']==33 and report['samples']==510
