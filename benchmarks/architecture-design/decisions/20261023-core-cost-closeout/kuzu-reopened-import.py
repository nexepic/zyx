"""COPY in <=1000-row transactions; split source prepared outside import timing."""
from pathlib import Path
import csv, json, os, sys, time
import kuzu
sys.path.insert(0,str(Path(__file__).resolve().parents[4]/'benchmarks/graphdb-comparison'))
from runner.adapters.kuzu import KuzuAdapter
source,target,scale,chunk_root=sys.argv[1:];source=Path(source);target=Path(target);chunk_root=Path(chunk_root)
assert not target.exists()
os.environ.update(KUZU_BENCH_BUFFER_POOL_BYTES=str(64*1024**2),KUZU_BENCH_MAX_DB_BYTES=str(2*1024**3))
chunks=json.loads((chunk_root/'chunks.json').read_text())
a=KuzuAdapter('kuzu',source,'medium' if scale=='holdout' else scale,'scan',1);a.db_path=target
start=time.perf_counter();a.setup();a.teardown()
for table,paths in chunks.items():
 for name in paths:
  path=str(chunk_root/name).replace("'","''")
  database=kuzu.Database(str(target),buffer_pool_size=64*1024**2,max_db_size=2*1024**3,max_num_threads=1)
  connection=kuzu.Connection(database,num_threads=1)
  result=connection.execute(f"COPY {table} FROM '{path}' (HEADER=true)")
  while result.has_next():result.get_next()
  result.close();connection.close();database.close()
milliseconds=(time.perf_counter()-start)*1000
import kuzu
db=kuzu.Database(str(target),buffer_pool_size=64*1024**2,max_db_size=2*1024**3,max_num_threads=1);con=kuzu.Connection(db,num_threads=1)
def scalar(q):
 result=con.execute(q)
 try:return int(result.get_next()[0])
 finally:result.close()
nodes=scalar('MATCH (n) RETURN count(n)');edges=scalar('MATCH ()-[r]->() WHERE r.weight>=0 RETURN count(r)');con.close();db.close()
print(json.dumps({'event':'sample','case':'fresh_import','mode':'equal_1000_row_commits','latency_ms':milliseconds,'loaded_rows':nodes+edges,'nodes':nodes,'edges':edges,'file_bytes':sum(p.stat().st_size for p in target.parent.glob(target.name+'*') if p.is_file())}),flush=True)
