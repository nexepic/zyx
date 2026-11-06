#!/usr/bin/env python3
"""Verify saved final-source discovery against every executed full-run case."""
import collections
import gzip
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent
suite = ''
discovered = []
with gzip.open(root / 'final-discovery.txt.gz', 'rt') as source:
    for line in source:
        if not line.strip():
            continue
        text = line.split('#')[0].strip()
        if line[0].isspace():
            discovered.append(suite + text)
        elif text.endswith('.'):
            suite = text
is_disabled = lambda name: any(p.startswith('DISABLED_') for p in name.split('.'))
enabled = {n for n in discovered if not is_disabled(n)}
disabled = {n for n in discovered if is_disabled(n)}
executions, failures, skipped, notrun, shards = [], [], [], [], []
for path in sorted(root.glob('final-shard-*.xml.gz')):
    with gzip.open(path, 'rb') as stream:
        document = ET.fromstring(stream.read())
    assert int(document.get('errors', 0)) == 0
    shards.append({'file': path.name, 'attributes': document.attrib})
    for case in document.iter('testcase'):
        name = case.get('classname') + '.' + case.get('name')
        if case.get('status') != 'run':
            notrun.append(name)
            continue
        executions.append(name)
        if case.find('failure') is not None:
            failures.append(name)
        if case.find('skipped') is not None:
            skipped.append(name)
counts = collections.Counter(executions)
stress = json.loads((root / 'stress-names.json').read_text())
result = {
    'source_commit': json.loads((root / 'verified-products.json').read_text())['source_commit'],
    'discovered': len(discovered), 'enabled': len(enabled), 'disabled': sorted(disabled),
    'executions': len(executions), 'distinct_executed': len(counts),
    'passed': len(executions) - len(failures) - len(skipped),
    'failures': failures, 'skipped': skipped,
    'missing': sorted(enabled - set(counts)), 'extra': sorted(set(counts) - enabled),
    'duplicates': {n: c for n, c in counts.items() if c != 1},
    'stress_counts': {n: counts[n] for n in stress}, 'shards': shards,
}
print(json.dumps(result, indent=2))
assert len(discovered) == len(set(discovered))
assert len(shards) == 4 and set(notrun) == disabled
assert not failures and not skipped and not result['missing'] and not result['extra'] and not result['duplicates']
assert all(counts[n] == 1 for n in stress)
