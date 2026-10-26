from pathlib import Path
import subprocess,json,os,hashlib,statistics,sys,csv,resource,shlex,shutil
from collections import Counter
r=Path(__file__).resolve().parents[4];d=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else r/'benchmarks/architecture-design/results/20261024-work-scan';b=Path(sys.argv[2]).resolve() if len(sys.argv)>2 else r/'build-scan-work';h=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance'
if (d/'initial-summary.json').exists(): raise RuntimeError('Initial evidence is frozen; use a fresh prepared run directory, never overwrite the control.')
resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3))
resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
cases=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
for scale in ['medium','holdout']:
 if not (d/scale/'manifest.json').is_file() or not (d/(scale+'.kuzu')).is_file(): raise RuntimeError('Prepare the registered CSV and Kuzu inputs in the fresh run directory first.')
recipe_dir=Path(__file__).resolve().parent
receipt=json.loads((recipe_dir/'receipt.json').read_text())
subprocess.run(['git','diff','--exit-code',receipt['base_head'],'--','src','include','apps','cmake','lib','CMakeLists.txt'],cwd=r,check=True,stdout=subprocess.DEVNULL)
for name,expected in receipt['csv_sha256'].items():
 if hashlib.file_digest((d/name).open('rb'),'sha256').hexdigest()!=expected: raise RuntimeError('Registered input hash mismatch: '+name)
def relocate(arg):
 for old,new in [('/workspace/zyx/build-scan-work',str(b)),('/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan',str(d)),('/workspace/zyx/',str(r)+'/')]: arg=arg.replace(old,new)
 return arg
# Remove test-hook dispatch from the only two translation units using it.
commands=subprocess.check_output(['ninja','-t','commands','zyx_core'],cwd=b,text=True).splitlines()
production_objects=[]
for source in ['src/storage/StorageIO.cpp','src/storage/relocatable/metadata/MetadataPagePool.cpp']:
 command=next(shlex.split(line) for line in commands if line.endswith(' -c '+str(r/source)))
 if '-DZYX_ENABLE_STORAGE_TEST_HOOKS=1' in command: command.remove('-DZYX_ENABLE_STORAGE_TEST_HOOKS=1')
 destination=d/Path(command[command.index('-o')+1]).name
 command[command.index('-o')+1]=str(destination)
 for flag in ['-MF','-MT']:
  if flag in command: command[command.index(flag)+1]=str(destination)+('.d' if flag=='-MF' else '')
 subprocess.run(command,cwd=b,check=True)
 production_objects.append(str(destination))
production_library=d/'libzyx-baseline-production.a';shutil.copyfile(b/'libzyx_core.a',production_library)
subprocess.run(['ar','r',str(production_library),*production_objects],check=True)
for name in ['public','import']:
 recipe=json.loads((recipe_dir/(name+'-recipe.json')).read_text())
 subprocess.run([relocate(arg) for arg in recipe['compile']],cwd=b,check=True)
 subprocess.run([str(production_library) if arg=='libzyx_core.a' else relocate(arg) for arg in recipe['link']],cwd=b,check=True)
for scale in ['medium','holdout']:
 with (d/(scale+'-zyx-import.log')).open('w') as out:
  manifest=json.loads((d/scale/'manifest.json').read_text());rows=sum(manifest['counts'].values())
  subprocess.run([str(d/'import-baseline'),str(d/scale),str(d/(scale+'.zyx')),str(rows)],stdout=out,stderr=subprocess.STDOUT,check=True)
summary=[]
for scale in ['medium','holdout']:
 users=list(csv.DictReader((d/scale/'users.csv').open()));posts=list(csv.DictReader((d/scale/'posts.csv').open()))
 expected={'label_scan_filter':[[['integer',sum(u['country']=='CN' for u in users)]]], 'all_nodes_property_filter':[[['integer',sum(float(u['score'])>=900 for u in users+posts)]]], 'aggregation_count_by_group':[[['string',key],['integer',count]] for key,count in Counter(u['country'] for u in users).items()], 'multi_key_group':[[['string',key[0]],['integer',key[1]],['integer',count]] for key,count in Counter((u['country'],int(u['age'])) for u in users).items()]}
 observed={}
 for proc in range(3):
  for engine in (['zyx','kuzu'] if proc%2==0 else ['kuzu','zyx']):
   command=([str(d/'public-baseline')] if engine=='zyx' else [sys.executable,str(h/'kuzu-worker.py')])+['read',str(d/(scale+'.'+engine)),str(d/(f'private-{scale}-{proc}-{engine}.db')),','.join(cases),'3','10']
   p=subprocess.run(command,text=True,capture_output=True,timeout=1200)
   (d/f'initial-{scale}-{proc}-{engine}.jsonl').write_text(p.stdout);(d/f'initial-{scale}-{proc}-{engine}.stderr').write_text(p.stderr)
   if p.returncode: raise RuntimeError(p.stderr)
   events=[json.loads(line) for line in p.stdout.splitlines() if line.startswith('{')]
   assert len(events)==40,(engine,len(events))
   for e in events:
    if e['case']=='multi_key_group': assert e['rows']==sorted(expected[e['case']],key=lambda row:(row[0][1],row[1][1])),e
    assert Counter(map(json.dumps,e['rows']))==Counter(map(json.dumps,expected[e['case']])),e
    observed.setdefault((engine,e['case']),[]).append(e['latency_ms'])
   print(scale,proc,engine,'passed',flush=True)
 for case in cases:
  z=statistics.median(observed['zyx',case]);k=statistics.median(observed['kuzu',case]);summary.append({'scale':scale,'case':case,'zyx_p50_ms':z,'kuzu_p50_ms':k,'zyx_over_kuzu':z/k})
(d/'initial-summary.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'results':summary,'threads':1,'processes':3,'warmup':3,'iterations':10,'oracle':'complete typed rows, unordered bags','new_environment_calibration':True,'source_library_sha256':hashlib.file_digest(production_library.open('rb'),'sha256').hexdigest(),'binary_sha256':hashlib.file_digest((d/'public-baseline').open('rb'),'sha256').hexdigest()},indent=2)+'\n')
print(json.dumps(summary),flush=True)
