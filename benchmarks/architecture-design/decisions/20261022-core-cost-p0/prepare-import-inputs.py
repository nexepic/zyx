from pathlib import Path
import csv,json,hashlib
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[3]
ROOT=REPO/'benchmarks/architecture-design/results/20261022-core-cost'
SOURCE=REPO/'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs'
TABLES=[('User','users'),('Post','posts'),('Tag','tags'),('FOLLOWS','follows'),('AUTHORED','authored'),('HAS_TAG','has_tag')]
for scale in ['medium','holdout']:
 target=ROOT/(scale+'-chunks');target.mkdir(exist_ok=False);manifest={};hashes={}
 for table,name in TABLES:
  manifest[table]=[]
  with (SOURCE/scale/(name+'.csv')).open(newline='') as f:
   reader=csv.reader(f);header=next(reader);batch=[];index=0
   def write():
    global index
    name=f'{table}-{index:05}.csv';index+=1
    with (target/name).open('w',newline='') as out:
     writer=csv.writer(out);writer.writerow(header);writer.writerows(batch)
    manifest[table].append(name);hashes[name]=hashlib.sha256((target/name).read_bytes()).hexdigest()
   for row in reader:
    batch.append(row)
    if len(batch)==1000:write();batch=[]
   if batch:write()
 (target/'chunks.json').write_text(json.dumps(manifest,indent=2)+'\n')
 (HERE/(scale+'-chunks-sha.json')).write_text(json.dumps(hashes,indent=2)+'\n')
 print(scale,sum(map(len,manifest.values())),'chunks',flush=True)
