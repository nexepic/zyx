"""One-query ABBA diagnostic after rejecting the dispersed primary window."""
from pathlib import Path
import hashlib
import json
import statistics
import sys
from process_receipt import run

R = Path('/workspace/zyx')
D = Path(__file__).resolve().parent
O = D / 'pinned-medium'
assert not O.exists(), 'Preserve existing diagnostic'
sys.path.insert(0, str(R / 'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign

def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()

source = campaign.INPUT_ROOT / 'medium.scan-corrected-v1.zyx'
binaries = {'control': campaign.INPUT_ROOT / 's4-native-write-cost/public-b-final',
            'candidate': D / 'public-a2-final'}
protocol = dict(formal_acceptance=False, scope='medium all-node query only', cpu=0,
                order=['control', 'candidate', 'candidate', 'control'], warmups=3,
                iterations=10, source_sha256=sha(source),
                binary_sha256={k: sha(v) for k, v in binaries.items()},
                quality={'each_worker_mad_over_median_max': .10,
                         'within_engine_process_median_max_over_min': 1.20},
                reason='Check repeatability with CPU placement and temporal drift controlled; no gate relaxation')
O.mkdir()
(O / 'protocol.json').write_text(json.dumps(protocol, indent=2) + '\n')
records = []
for index, engine in enumerate(protocol['order']):
    stem = O / f'{index}-{engine}'
    target = O / f'{index}-{engine}.db'
    command = ['/usr/bin/taskset', '-c', '0', str(binaries[engine]), 'read', str(source),
               str(target), 'all_nodes_property_filter', '3', '10']
    print('START', index, engine, flush=True)
    with stem.with_suffix('.jsonl').open('w') as out, stem.with_suffix('.stderr').open('w') as err:
        resources = run(command, out, err, campaign.ENV, campaign.REG['limits'])
    events = [json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines()]
    assert resources['returncode'] == 0 and len(events) == 10, resources
    for event in events:
        campaign.check_event('all_nodes_property_filter', event, 'medium', False)
    values = [x['latency_ms'] for x in events]
    median = statistics.median(values)
    mad = statistics.median(abs(x - median) for x in values)
    record = dict(engine=engine, index=index, command=command, oracle='10 passed',
                  median_ms=median, mad_over_median=mad / median, resources=resources)
    records.append(record)
    stem.with_suffix('.receipt.json').write_text(json.dumps(record, indent=2) + '\n')
    print('PASS', index, engine, median, flush=True)
spread = {}
for engine in binaries:
    medians = [x['median_ms'] for x in records if x['engine'] == engine]
    spread[engine] = max(medians) / min(medians)
quality = all(x['mad_over_median'] <= .10 for x in records) and all(x <= 1.20 for x in spread.values())
assert sha(source) == protocol['source_sha256']
for engine, binary in binaries.items():
    assert sha(binary) == protocol['binary_sha256'][engine]
(O / 'summary.json').write_text(json.dumps(dict(records=records, process_spread=spread,
    quality_passed=quality, formal_acceptance=False), indent=2) + '\n')
print('COMPLETE quality', quality, flush=True)
