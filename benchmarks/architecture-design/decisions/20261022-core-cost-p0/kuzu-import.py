from pathlib import Path
import sys,os,time,json
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
from runner.adapters.kuzu import KuzuAdapter
source,target,scale=sys.argv[1:];source=Path(source);target=Path(target)
assert not target.exists();os.environ.update(KUZU_BENCH_BUFFER_POOL_BYTES=str(64*1024**2),KUZU_BENCH_MAX_DB_BYTES=str(2*1024**3))
a=KuzuAdapter('kuzu',source,'medium' if scale=='holdout' else scale,'scan',1);a.db_path=target
start=time.perf_counter();a.setup();rows=a.load_nodes_edges();a.teardown();milliseconds=(time.perf_counter()-start)*1000
import kuzu
db=kuzu.Database(str(target),buffer_pool_size=64*1024**2,max_db_size=2*1024**3,max_num_threads=1);con=kuzu.Connection(db,num_threads=1)
def scalar(query):
 result=con.execute(query)
 try:return int(result.get_next()[0])
 finally:result.close()
nodes=scalar('MATCH (n) RETURN count(n)');edges=scalar('MATCH ()-[r]->() WHERE r.weight>=0 RETURN count(r)');con.close();db.close()
print(json.dumps({'event':'sample','case':'fresh_import','latency_ms':milliseconds,'loaded_rows':rows,'nodes':nodes,'edges':edges,'file_bytes':sum(p.stat().st_size for p in target.parent.glob(target.name+'*') if p.is_file())}),flush=True)
