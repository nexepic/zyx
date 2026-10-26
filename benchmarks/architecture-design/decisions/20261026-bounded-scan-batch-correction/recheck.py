"""The single bounded corrective S2 recheck; never rerun the S5 matrix here."""
from pathlib import Path
import csv
import hashlib
import json
import math
import statistics
import subprocess
import sys

REPO = Path('/workspace/zyx')
HERE = Path(__file__).resolve().parent
PRIOR = HERE.parent / '20261025-final-scan-comparison'
sys.path.insert(0, str(PRIOR))
import campaign

ROOT = campaign.INPUT_ROOT / 'bounded-correction'
FREEZE = ROOT / 'freeze.json'
CASES = ['label_scan_filter', 'all_nodes_property_filter',
         'aggregation_count_by_group', 'multi_key_group']

def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def freeze():
    assert not FREEZE.exists(), 'Never overwrite a frozen corrective campaign'
    tests = json.loads((HERE / 'related-83-tests.json').read_text())
    assert tests['tests'] == 83 and tests['failures'] == 0
    compatible = json.loads((HERE / 'compatible-read-tests.json').read_text())
    assert compatible['tests'] > 0 and compatible['failures'] == 0
    artifacts = {}
    old = json.loads((campaign.ROOT / 'freeze.json').read_text())
    for name in ['initial_binary', 'control_binary', 'initial_library', 'control_library',
                 'medium_row', 'holdout_row', 'medium_kuzu', 'holdout_kuzu',
                 'public_probe_source', 'oracle_source', 'kuzu_worker_source']:
        item = old['artifacts'][name]
        assert sha(item['path']) == item['sha256'], name
        artifacts[name] = item
    paths = {'B_binary': Path('/tmp/zyx-corrected-b-public'),
             'B_values': Path('/tmp/zyx-corrected-b-values'),
             'B_library': REPO / 'build-scan-candidate/libzyx_core.a',
             'campaign': PRIOR / 'campaign.py', 'recheck': Path(__file__),
             'tests': HERE / 'related-83-tests.json',
             'compatible_tests': HERE / 'compatible-read-tests.json',
             'values_source': PRIOR / 'value-probe.cpp',
             'untimed_work_script': HERE / 'check-work.py'}
    for scale in ['medium', 'holdout']:
        paths[scale + '_scan'] = campaign.INPUT_ROOT / f'{scale}.scan-corrected-v1.zyx'
        receipt = json.loads((ROOT / f'{scale}-derived-receipt.json').read_text())
        assert receipt['returncode'] == 0
        assert receipt['source_sha256_before'] == receipt['source_sha256_after']
        assert sha(paths[scale + '_scan']) == receipt['target_sha256']
        assert paths[scale + '_scan'].stat().st_size * 10 <= Path(artifacts[scale + '_row']['path']).stat().st_size * 9
        for family in ['primary', 'edge-batch', 'edge-single']:
            observation = ROOT / f'untimed-{scale}-{family}.json'
            assert json.loads(observation.read_text())['status'] == 'passed'
            paths[f'{scale}_{family}_oracle'] = observation
        for path in (campaign.INPUT_ROOT / scale).glob('*.csv'):
            paths[scale + '_' + path.name] = path
    for name, path in paths.items():
        artifacts[name] = {'path': str(path), 'bytes': path.stat().st_size, 'sha256': sha(path)}
    sources = {}
    for tree in ['include', 'src', 'cmake', 'apps/scan_convert']:
        for path in (REPO / tree).rglob('*'):
            if path.is_file() and path.suffix in ['.cpp', '.hpp', '.h', '.cmake']:
                sources[str(path.relative_to(REPO))] = sha(path)
    sources['CMakeLists.txt'] = sha(REPO / 'CMakeLists.txt')
    (ROOT / 'source-files.json').write_text(json.dumps(sources, indent=2) + '\n')
    version = subprocess.check_output([campaign.PYTHON, '-c', 'import kuzu;print(kuzu.__version__)'], text=True).strip()
    assert version == '0.11.3'
    FREEZE.write_text(json.dumps({'artifacts': artifacts, 'kuzu_version': version,
        'original_failed_freeze_sha256': sha(campaign.ROOT / 'freeze.json'),
        'correction_number': 1, 'protocol': campaign.REG,
        'scope': 'Corrected B versus unchanged initial/control/Kuzu; A already failed, no extra candidate',
        'full_s5_matrix': False}, indent=2) + '\n')

def run():
    frozen = json.loads(FREEZE.read_text())
    for name, item in frozen['artifacts'].items():
        assert sha(item['path']) == item['sha256'], name
    for name, expected in json.loads((ROOT / 'source-files.json').read_text()).items():
        assert sha(REPO / name) == expected, name
    campaign.ROOT = ROOT
    campaign.OUT = ROOT / 'workers'
    campaign.OUT.mkdir(exist_ok=True)
    campaign.FREEZE_ID = sha(FREEZE)
    campaign.BINARIES['B'] = frozen['artifacts']['B_binary']['path']
    campaign.B_IMAGES.update({scale: Path(frozen['artifacts'][scale + '_scan']['path']) for scale in ['medium', 'holdout']})
    for number in range(3):
        order = ['initial', 'control', 'B', 'kuzu'] if number % 2 == 0 else ['kuzu', 'B', 'control', 'initial']
        for scale in ['medium', 'holdout']:
            for engine in order:
                campaign.worker(engine, scale, 'scan', CASES, number)
    rows = {}
    for record in campaign.results:
        for event in record['samples']:
            rows.setdefault((record['scale'], event['case'], record['engine']), []).append(event['latency_ms'])
    summary = {key: {'samples': len(values), 'p50_ms': statistics.median(values),
                    'p95_ms': sorted(values)[math.ceil(.95 * len(values)) - 1]}
               for key, values in rows.items()}
    with (ROOT / 'selection-summary.csv').open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['scale', 'case', 'engine', 'samples', 'p50_ms', 'p95_ms'])
        for key, value in sorted(summary.items()):
            writer.writerow([*key, value['samples'], value['p50_ms'], value['p95_ms']])
    gates = []
    for scale in ['medium', 'holdout']:
        for case in CASES:
            values = {engine: summary.get((scale, case, engine)) for engine in ['initial', 'control', 'B', 'kuzu']}
            valid = all(value and value['samples'] == 30 for value in values.values())
            speedups = {engine: values[engine]['p50_ms'] / values['B']['p50_ms'] for engine in ['initial', 'control']} if valid else {}
            gates.append({'scale': scale, 'case': case, 'valid': valid, 'speedup': speedups,
                          'passes_5x_each_control': valid and all(value >= 5 for value in speedups.values()),
                          'candidate_p50_ms': values['B']['p50_ms'] if values['B'] else None,
                          'kuzu_p50_ms': values['kuzu']['p50_ms'] if values['kuzu'] else None,
                          'beats_kuzu': valid and values['B']['p50_ms'] < values['kuzu']['p50_ms']})
    report = {'freeze_sha256': campaign.FREEZE_ID, 'primary': gates,
              'processes': len(campaign.results), 'samples': sum(len(r['samples']) for r in campaign.results),
              'passes_all_primary': all(g['passes_5x_each_control'] for g in gates),
              'beats_kuzu_all_primary': all(g['beats_kuzu'] for g in gates),
              's5_complete': False}
    (ROOT / 'selection-gate.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)

if __name__ == '__main__':
    {'freeze': freeze, 'run': run}[sys.argv[1]]()
