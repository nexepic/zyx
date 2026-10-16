from pathlib import Path
from collections import defaultdict
import importlib.util,subprocess,json,statistics,os,signal
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
assert (w.OUT/'registration.json').exists()
root=w.OUT/'mechanisms';root.mkdir(exist_ok=False)
expected={'native_selected_storage':4096,'typed_filter_including_conversion':2000,'generic_filter_same_source':2000,'native_full_sort_all_output':4096,'native_topk_128_output':128}
reports=[]
for number in range(1,4):
 folder=root/f'round-{number}';folder.mkdir()
 with (folder/'raw.jsonl').open('w') as output,(folder/'stderr.txt').open('w') as error:
  process=subprocess.Popen(['/tmp/zyx-mechanism-probe',str(folder/'private.zyx')],stdout=output,stderr=error,start_new_session=True)
  try:status=process.wait(timeout=1800)
  except BaseException:
   try:os.killpg(process.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   process.wait(timeout=5);raise
 if status:raise RuntimeError(f'mechanism correctness failed: {folder}')
 values=[json.loads(line) for line in (folder/'raw.jsonl').read_text().splitlines()];assert len(values)==50
 groups=defaultdict(list)
 for event in values:
  assert event['validated_rows']==expected[event['mechanism']]
  groups[event['mechanism']].append(event)
 assert set(groups)==set(expected)
 for name,events in groups.items():
  assert sorted(v['sample'] for v in events)==list(range(10))
  reports.append({'round':number,'mechanism':name,'p50_ms':statistics.median(v['milliseconds'] for v in events),'validated_rows':expected[name]})
 print(json.dumps({'phase':'mechanism','round_completed':number,'samples':50,'full_values_checked':True}),flush=True)
summary={name:{'median_process_p50_ms':statistics.median(v['p50_ms'] for v in reports if v['mechanism']==name),'process_p50_ms':[v['p50_ms'] for v in reports if v['mechanism']==name]} for name in expected}
(root/'report.json').write_text(json.dumps({'scope':'mechanism falsification only, not end-to-end acceptance','independent_processes':3,'warmups':3,'samples_per_mechanism_per_process':10,'correctness':'all 4096 native property values, every filter selection and every requested sort output/order checked','sort_limit':'full sort returns all 4096 rows; Top-K returns 128. Cardinalities differ and this is not a strict end-to-end ratio.','filter_limit':'typed includes conversion; generic uses the identical already materialized source','results':summary,'processes':reports},indent=2)+'\n')
