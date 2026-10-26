"""Summarize construction resources separately from query measurement."""
from pathlib import Path
import json
ROOT = Path('/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan')
records = []
for path in sorted((ROOT / 'diagnostics').glob('v*-construction/*-resources.json')):
    record = json.loads(path.read_text())
    records.append({'evidence': str(path.relative_to(ROOT)), **record})
report = {
    'query_benchmark': False,
    'attempts': records,
    'recorded_attempt_totals': {
        'wall_seconds_sum': sum(r['wall_seconds'] for r in records),
        'os_write_bytes_sum': sum(r['os_write_bytes'] for r in records),
        'peak_process_rss_bytes': max((r['peak_rss_bytes'] for r in records), default=0),
    },
    'accounting_limits': {
        'source_workspace_bytes': 1048576,
        'maintenance_workspace_bytes': 7340032,
        'raw_cache_bytes': 67108864,
        'metadata_cache_bytes': 1048576,
        'metadata_cache_entries': 128,
        'additional_cache_bytes': 0,
        'combined_file_length_bytes': 2147483648,
    },
    'limitations': [
        'Sum of worker wall times is not elapsed campaign time; workers may overlap.',
        'OS write bytes include repeated writes and are not peak file length.',
        'File length is bounded by construction admission; these receipts do not sample its exact peak.',
        'Earlier attempts without resource receipts are excluded from numeric totals and remain in diagnostics.',
        'Failed or stopped construction attempts are retained in these totals, not presented as accepted throughput.',
    ],
    'admitted_images': [],
}
for scale, version in [('medium', 'v12'), ('holdout', 'v11')]:
    source = ROOT / (scale + '.zyx')
    target = ROOT / (scale + '.scan-private-' + version + '.zyx')
    resources = ROOT / ('diagnostics/' + version + '-construction/' + scale + '-resources.json')
    if not resources.exists():
        report['admitted_images'].append({'scale': scale, 'status': 'pending'})
        continue
    assert json.loads(resources.read_text())['returncode'] == 0, scale
    source_bytes, target_bytes = source.stat().st_size, target.stat().st_size
    assert target_bytes * 10 <= source_bytes * 9, scale
    report['admitted_images'].append({
        'scale': scale, 'status': 'completed', 'source_bytes': source_bytes,
        'target_bytes': target_bytes, 'saving_fraction': 1 - target_bytes / source_bytes,
        'construction_evidence': str(resources.relative_to(ROOT)),
    })
output = ROOT / 'final-comparison/construction-costs.json'
output.parent.mkdir(exist_ok=True)
output.write_text(json.dumps(report, indent=2) + '\n')
print(output)
