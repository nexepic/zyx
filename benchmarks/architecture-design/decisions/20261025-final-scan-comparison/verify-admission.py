"""One untimed public correctness check after successful real-image admission."""
from pathlib import Path
import json, subprocess, sys
import campaign
scale = sys.argv[1]
assert scale in ['medium', 'holdout']
version = {'medium': 'v12', 'holdout': 'v11'}[scale]
root = campaign.INPUT_ROOT
resources = root / ('diagnostics/' + version + '-construction/' + scale + '-resources.json')
assert json.loads(resources.read_text())['returncode'] == 0, 'construction did not complete'
cases = ['label_scan_filter', 'all_nodes_property_filter', 'aggregation_count_by_group', 'multi_key_group']
source = root / (scale + '.scan-private-' + version + '.zyx')
target = Path('/tmp') / ('zyx-' + scale + '-public-ready-check.zyx')
assert not target.exists()
stem = root / ('untimed-public-' + scale + '-check')
command = ['/tmp/zyx-final-b-public', 'check-read', str(source), str(target), ','.join(cases), '0', '1']
with stem.with_suffix('.jsonl').open('w') as out, stem.with_suffix('.stderr').open('w') as err:
    result = subprocess.run(command, env=campaign.ENV, stdout=out, stderr=err,
                            preexec_fn=campaign.limits, timeout=1200)
assert result.returncode == 0, ('public check failed', result.returncode)
events = [json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines() if line.startswith('{')]
assert len(events) == len(cases)
assert sorted(e.get('case') for e in events) == sorted(cases)
for event in events:
    assert event.get('status') != 'failed' and 'latency_ms' not in event
    campaign.check_event(event['case'], event, scale)
assert not target.exists(), 'successful read worker did not remove its private clone'
receipt = {'status': 'passed', 'cases': cases, 'query_benchmark': False,
           'oracle': 'complete typed rows, multiplicity, ordered multi-key groups', 'command': command}
(root / ('untimed-public-' + scale + '-check-receipt.json')).write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(receipt))
