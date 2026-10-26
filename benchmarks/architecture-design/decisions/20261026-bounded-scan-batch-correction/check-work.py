"""Untimed corrected-image oracle and physical-work observations."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

REPO = Path('/workspace/zyx')
PRIOR = REPO / 'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'
sys.path.insert(0, str(PRIOR))
import campaign

OUT = campaign.INPUT_ROOT / 'bounded-correction'
CASES = ['label_scan_filter', 'all_nodes_property_filter',
         'aggregation_count_by_group', 'multi_key_group']

for scale in ['medium', 'holdout']:
    source = campaign.INPUT_ROOT / f'{scale}.scan-corrected-v1.zyx'
    before = hashlib.file_digest(source.open('rb'), 'sha256').hexdigest()
    for family, cases, mode in [('primary', CASES, 'check-read'),
                                ('edge-batch', ['edge_batch_1000'], 'check-write'),
                                ('edge-single', ['edge_create_delete'], 'check-write')]:
        stem = OUT / f'untimed-{scale}-{family}'
        assert not stem.with_suffix('.json').exists(), stem
        target = stem.with_suffix('.db')
        assert not target.exists(), target
        command = ['/tmp/zyx-corrected-b-values', mode, str(source), str(target),
                   ','.join(cases), '0', '1']
        print('START untimed', scale, family, flush=True)
        with stem.with_suffix('.jsonl').open('w') as out, stem.with_suffix('.stderr').open('w') as err:
            result = subprocess.run(command, env=campaign.ENV, stdout=out, stderr=err,
                                    preexec_fn=campaign.limits, timeout=1200)
        assert result.returncode == 0, stem
        events = [json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines()
                  if line.startswith('{')]
        assert len(events) == len(cases), stem
        for event in events:
            assert 'latency_ms' not in event and event.get('status') != 'failed', stem
            campaign.check_event(event['case'], event, scale, mode == 'check-write')
        assert hashlib.file_digest(source.open('rb'), 'sha256').hexdigest() == before
        stem.with_suffix('.json').write_text(json.dumps({
            'query_benchmark': False, 'status': 'passed', 'command': command,
            'source_sha256': before, 'full_result_oracle': True,
        }, indent=2) + '\n')
        for path in target.parent.glob(target.name + '*'):
            if path.is_file():
                path.unlink()
        print('FINISH untimed', scale, family, flush=True)
