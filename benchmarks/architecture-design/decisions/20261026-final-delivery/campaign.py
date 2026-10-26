"""Final selected-path window, reusing the original public worker and oracles."""
from pathlib import Path
import csv
import hashlib
import json
import math
import platform
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = Path('/workspace/zyx')
sys.path.insert(0, str(HERE.parent / '20261025-final-scan-comparison'))
import campaign as original

ROOT = original.INPUT_ROOT / 'final-delivery'
ROOT.mkdir(exist_ok=True)
TOOLS = original.INPUT_ROOT / 's3-lifecycle'
S3 = HERE.parent / '20261026-s3-production-lifecycle'
PRIMARY = ['label_scan_filter', 'all_nodes_property_filter',
           'aggregation_count_by_group', 'multi_key_group']
RETRIEVAL = ['point_lookup_indexed', 'graph_full_point', 'graph_full_neighbors',
             'one_hop_expand', 'two_hop_expand']
WRITES = ['property_set', 'edge_batch_1000', 'property_noop', 'edge_create_delete']

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def freeze():
    assert not (ROOT / 'freeze.json').exists(), 'Never overwrite frozen final evidence'
    acceptance = json.loads((S3 / 'acceptance.json').read_text())
    assert acceptance['status'] == 'accepted' and acceptance['accepted_progress_percent'] == 65
    for name, count in [('candidate', 108), ('default', 25), ('compat', 89)]:
        tests = json.loads((S3 / f'{name}-tests.json').read_text())
        assert tests['tests'] == count and tests['failures'] == 0
    general = json.loads((S3 / 'generality-receipt.json').read_text())
    assert general['returncode'] == 0 and general['source_sha256_before'] == general['source_sha256_after']
    paths = {'B_binary': TOOLS / 'public-b', 'legacy_binary': TOOLS / 'public-legacy',
             'initial_binary': original.INPUT_ROOT / 'public-baseline',
             'control_binary': TOOLS / 'public-control',
             'B_library': REPO / 'build-scan-candidate/libzyx_core.a',
             'legacy_library': REPO / 'build-scan-row-production/libzyx_core.a',
             'control_library': Path('/workspace/zyx-frozen-control-369039d/build/libzyx_core.a'),
             'initial_library': original.INPUT_ROOT / 'libzyx-baseline-production.a',
             'public_probe_source': HERE.parent / '20261023-core-cost-acceptance/public-probe.cpp',
             'oracle': HERE.parent / '20261023-core-cost-acceptance/oracle.py',
             'kuzu_worker': HERE.parent / '20261023-core-cost-acceptance/kuzu-worker.py',
             'worker_source': HERE.parent / '20261025-final-scan-comparison/campaign.py',
             'campaign_source': Path(__file__), 'protocol': HERE / 'protocol.json',
             'S3_acceptance': S3 / 'acceptance.json',
             'large_guard': original.INPUT_ROOT / 'guard-frozen-large.zyx',
             'skew_guard': original.INPUT_ROOT / 'guard-skew-indexed.zyx'}
    old = json.loads((original.ROOT / 'freeze.json').read_text())
    for key in ['initial_binary', 'initial_library', 'control_library', 'public_probe_source', 'oracle', 'kuzu_worker']:
        old_key = {'oracle': 'oracle_source', 'kuzu_worker': 'kuzu_worker_source'}.get(key, key)
        assert sha(paths[key]) == old['artifacts'][old_key]['sha256'], key
    for scale in ['medium', 'holdout']:
        for kind, suffix in [('row', '.zyx'), ('scan', '.scan-corrected-v1.zyx'), ('kuzu', '.kuzu')]:
            paths[f'{scale}_{kind}'] = original.INPUT_ROOT / (scale + suffix)
        assert paths[f'{scale}_scan'].stat().st_size * 10 <= paths[f'{scale}_row'].stat().st_size * 9
        for family in ['primary', *WRITES]:
            receipt = TOOLS / f'public-{scale}-{family}.json'
            assert json.loads(receipt.read_text())['status'] == 'passed'
            paths[f'{scale}_{family}_oracle'] = receipt
        for path in (original.INPUT_ROOT / scale).glob('*.csv'):
            paths[scale + '_' + path.name] = path
    artifacts = {key: {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}
                 for key, path in paths.items()}
    sources = {}
    for tree in ['include', 'src', 'cmake', 'apps/scan_convert']:
        for path in (REPO / tree).rglob('*'):
            if path.is_file() and path.suffix in ['.cpp', '.hpp', '.h', '.cmake']:
                sources[str(path.relative_to(REPO))] = sha(path)
    sources['CMakeLists.txt'] = sha(REPO / 'CMakeLists.txt')
    (ROOT / 'source-files.json').write_text(json.dumps(sources, indent=2) + '\n')
    version = subprocess.check_output([original.PYTHON, '-c', 'import kuzu;print(kuzu.__version__)'], text=True).strip()
    assert version == '0.11.3'
    record = {'artifacts': artifacts, 'protocol': json.loads((HERE / 'protocol.json').read_text()),
              'head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=REPO, text=True).strip(),
              'kuzu_version': version, 'platform': platform.platform(),
              'cpu_identity': subprocess.check_output(['lscpu'], text=True),
              'cpu_quota': Path('/sys/fs/cgroup/cpu.max').read_text().strip(),
              'historical_latency_reused': False, 'fixed_5x_gate_waived_by_user': True}
    (ROOT / 'freeze.json').write_text(json.dumps(record, indent=2) + '\n')

def summarize():
    rows = {}
    for record in original.results:
        for event in record['samples']:
            rows.setdefault((record['scale'], event['case'], record['engine']), []).append(event['latency_ms'])
    summary = {key: {'samples': len(values), 'p50_ms': statistics.median(values),
                    'p95_ms': sorted(values)[math.ceil(.95 * len(values)) - 1]}
               for key, values in rows.items()}
    with (ROOT / 'summary.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['scale', 'case', 'engine', 'samples', 'p50_ms', 'p95_ms'])
        for key, stats in sorted(summary.items()):
            writer.writerow([*key, stats['samples'], stats['p50_ms'], stats['p95_ms']])
    with (ROOT / 'process-summary.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['scale', 'case', 'engine', 'round', 'samples', 'p50_ms', 'p95_ms'])
        for record in original.results:
            for case, stats in record['case_statistics'].items():
                writer.writerow([record['scale'], case, record['engine'], record['round'], stats['samples'], stats['p50_ms'], stats['p95_ms']])
    primary = []
    for scale in ['medium', 'holdout']:
        for case in PRIMARY:
            group = {engine: summary.get((scale, case, engine)) for engine in ['initial', 'control', 'B', 'kuzu']}
            valid = all(value and value['samples'] == 30 for value in group.values())
            speedups = {engine: group[engine]['p50_ms'] / group['B']['p50_ms'] for engine in ['initial', 'control']} if valid else {}
            primary.append({'scale': scale, 'case': case, 'valid': valid, 'speedup': speedups,
                            'real_gain_both_controls': valid and all(value > 1 for value in speedups.values()),
                            'candidate_p50_ms': group['B']['p50_ms'] if group['B'] else None,
                            'kuzu_p50_ms': group['kuzu']['p50_ms'] if group['kuzu'] else None,
                            'beats_kuzu': valid and group['B']['p50_ms'] < group['kuzu']['p50_ms']})
    expected = [(scale, case, engine) for scale in ['medium', 'holdout'] for case in RETRIEVAL + WRITES for engine in ['B', 'legacy']]
    expected += [(scale, case, 'legacy') for scale, cases in [('large', ['large_selected', 'large_full', 'large_patch']), ('skew', ['skew_out', 'skew_both', 'skew_twohop'])] for case in cases]
    guards = []
    for scale, case, engine in expected:
        candidate = summary.get((scale, case, engine))
        controls = {name: summary.get((scale, case, name)) for name in ['initial', 'control']}
        valid = bool(candidate and candidate['samples'] == 30 and all(value and value['samples'] == 30 for value in controls.values()))
        ratios = {name: {metric: candidate[metric] / value[metric] for metric in ['p50_ms', 'p95_ms']} for name, value in controls.items()} if valid else {}
        guards.append({'scale': scale, 'case': case, 'engine': engine, 'valid': valid, 'ratios': ratios,
                       'confirmation_required': not valid or any(value > 1.05 for metrics in ratios.values() for value in metrics.values())})
    complete = len(original.results) == 171 and sum(len(record['samples']) for record in original.results) == 3660 and all(record['status'] == 'passed' for record in original.results)
    report = {'freeze_sha256': original.FREEZE_ID, 'processes': len(original.results),
              'measured_samples': sum(len(record['samples']) for record in original.results),
              'measurement_complete': complete, 'primary': primary, 'guards': guards,
              'acceptance': 'Requires stable guard review; no automatic 100% or Kuzu superiority claim'}
    (ROOT / 'evaluation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'measurement_complete': complete, 'processes': len(original.results),
                      'measured_samples': report['measured_samples'],
                      'suspect_guards': sum(guard['confirmation_required'] for guard in guards)}), flush=True)

def run():
    frozen = json.loads((ROOT / 'freeze.json').read_text())
    for key, item in frozen['artifacts'].items():
        assert sha(item['path']) == item['sha256'], key
    for path, expected in json.loads((ROOT / 'source-files.json').read_text()).items():
        assert sha(REPO / path) == expected, path
    original.ROOT = ROOT
    original.OUT = ROOT / 'workers'
    original.OUT.mkdir(exist_ok=True)
    original.FREEZE_ID = sha(ROOT / 'freeze.json')
    original.BINARIES.update({engine: frozen['artifacts'][engine + '_binary']['path'] for engine in ['initial', 'control', 'legacy', 'B']})
    original.B_IMAGES.update({scale: Path(frozen['artifacts'][scale + '_scan']['path']) for scale in ['medium', 'holdout']})
    for number in range(3):
        order = ['initial', 'control', 'legacy', 'B', 'kuzu'] if number % 2 == 0 else ['kuzu', 'B', 'legacy', 'control', 'initial']
        for scale in ['medium', 'holdout']:
            for engine in [value for value in order if value != 'legacy']:
                original.worker(engine, scale, 'scan', PRIMARY, number)
            for engine in [value for value in order if value != 'kuzu']:
                original.worker(engine, scale, 'retrieval', RETRIEVAL, number)
            for case in WRITES:
                for engine in [value for value in order if value != 'kuzu']:
                    original.worker(engine, scale, case, [case], number, 'write')
        for scale, cases in [('large', ['large_selected', 'large_full']), ('skew', ['skew_out', 'skew_both', 'skew_twohop'])]:
            for engine in ['initial', 'control', 'legacy'] if number % 2 == 0 else ['legacy', 'control', 'initial']:
                original.worker(engine, scale, 'special-read', cases, number)
                if scale == 'large':
                    original.worker(engine, scale, 'large_patch', ['large_patch'], number, 'write')
    summarize()

if __name__ == '__main__':
    {'freeze': freeze, 'run': run}[sys.argv[1]]()
