from pathlib import Path
import kuzu,re,json
D=Path(__file__).resolve().parent;R=D.parents[3]
source=R/'benchmarks/architecture-design/results/20261024-work-scan/medium.kuzu'
text=(R/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance/public-probe.cpp').read_text()
queries={n:json.loads('"'+s+'"') for n,s in re.findall(r'if\(name=="([^"]+)"\)return "((?:[^"\\]|\\.)*)";',text)}
db=kuzu.Database(str(source),buffer_pool_size=64*1024*1024,read_only=True,max_num_threads=1)
c=kuzu.Connection(db,num_threads=1)
plans={}
for case in ('all_nodes_property_filter','label_scan_filter'):
 r=c.execute('EXPLAIN '+queries[case]);rows=[]
 while r.has_next():rows.append(r.get_next())
 r.close();plans[case]={'query':queries[case],'explain':rows,'actual_skipped_blocks':'not measured'}
c.close();db.close()
(D/'kuzu-plans.json').write_text(json.dumps(plans,indent=2)+'\n')
