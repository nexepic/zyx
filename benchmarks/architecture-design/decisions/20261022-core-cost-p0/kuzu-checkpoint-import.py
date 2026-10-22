"""Equal <=1000-row commits using Kuzu parameterized bulk CREATE, without COPY allocator accumulation."""
from pathlib import Path
import csv,json,os,sys,time
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/'benchmarks/graphdb-comparison'))
from runner.adapters.kuzu import KuzuAdapter
source,target,scale=sys.argv[1:];source=Path(source);target=Path(target)
assert not target.exists()
os.environ.update(KUZU_BENCH_BUFFER_POOL_BYTES=str(64*1024**2),KUZU_BENCH_MAX_DB_BYTES=str(2*1024**3))
a=KuzuAdapter('kuzu',source,'medium' if scale=='holdout' else scale,'scan',1);a.db_path=target
statements=[('users',"UNWIND $rows AS r CREATE (:User {id:r.id,age:r.age,country:r.country,score:r.score}) RETURN count(*)",{'age':int,'score':float}),('posts',"UNWIND $rows AS r CREATE (:Post {id:r.id,created_at:r.created_at,score:r.score}) RETURN count(*)",{'created_at':int,'score':float}),('tags',"UNWIND $rows AS r CREATE (:Tag {id:r.id,rank:r.rank}) RETURN count(*)",{'rank':int}),('follows',"UNWIND $rows AS r MATCH (a:User {id:r.src}),(b:User {id:r.dst}) CREATE (a)-[:FOLLOWS {weight:r.weight}]->(b) RETURN count(*)",{'weight':int}),('authored',"UNWIND $rows AS r MATCH (a:User {id:r.src}),(b:Post {id:r.dst}) CREATE (a)-[:AUTHORED {weight:r.weight}]->(b) RETURN count(*)",{'weight':int}),('has_tag',"UNWIND $rows AS r MATCH (a:Post {id:r.src}),(b:Tag {id:r.dst}) CREATE (a)-[:HAS_TAG {weight:r.weight}]->(b) RETURN count(*)",{'weight':int})]
start=time.perf_counter();a.setup();loaded=0;commits=0
for filename,query,converters in statements:
 with (source/(filename+'.csv')).open(newline='') as f:
  rows=[{k:converters[k](v) if k in converters else v for k,v in row.items()} for row in csv.DictReader(f)]
 for begin in range(0,len(rows),1000):
  batch=rows[begin:begin+1000];result=a._connection.execute(query,{'rows':batch})
  try:
   count=result.get_next()[0];assert count==len(batch),(filename,count,len(batch));loaded+=count
  finally:result.close()
  commits+=1
  a._execute_statement("CHECKPOINT")
 del rows
a.teardown();ms=(time.perf_counter()-start)*1000
import kuzu
db=kuzu.Database(str(target),buffer_pool_size=64*1024**2,max_db_size=2*1024**3,max_num_threads=1);con=kuzu.Connection(db,num_threads=1)
def scalar(q):
 result=con.execute(q)
 try:return int(result.get_next()[0])
 finally:result.close()
nodes=scalar('MATCH (n) RETURN count(n)');edges=scalar('MATCH ()-[r]->() WHERE r.weight>=0 RETURN count(r)');con.close();db.close()
print(json.dumps({'event':'sample','case':'fresh_import','mode':'equal_1000_row_commits','latency_ms':ms,'loaded_rows':loaded,'commits':commits,'nodes':nodes,'edges':edges,'file_bytes':sum(p.stat().st_size for p in target.parent.glob(target.name+'*') if p.is_file())}),flush=True)
