#!/usr/bin/env python3
"""Recheck the saved full run without requiring the original build directory."""
import collections
import gzip
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent
expected = json.loads((root / 'final-full-exact-once.json').read_text())
products = json.loads((root / 'verified-products.json').read_text())
suite = ''
discovered = []
with gzip.open(root / 'final-discovery.txt.gz', 'rt') as source:
    for line in source:
        if not line.strip():
            continue
        if line[0].isspace():
            discovered.append(suite + line.strip().split('#')[0].strip())
        else:
            suite = line.split('#')[0].strip()


def disabled(name):
    return any(part.startswith('DISABLED_') for part in name.split('.'))


enabled = {name for name in discovered if not disabled(name)}
disabled_names = {name for name in discovered if disabled(name)}
executed, failed, skipped, notrun, shards = [], [], [], [], []
for path in sorted(root.glob('final-shard-*.xml.gz')):
    with gzip.open(path, 'rb') as source:
        raw = source.read()
    document = ET.fromstring(raw)
    assert int(document.get('errors', 0)) == 0
    shards.append({'file': path.name, 'xml_sha256': hashlib.sha256(raw).hexdigest(),
                   'attributes': document.attrib})
    for case in document.iter('testcase'):
        name = case.get('classname') + '.' + case.get('name')
        if case.get('status') != 'run':
            notrun.append(name)
            continue
        executed.append(name)
        if case.find('failure') is not None:
            failed.append(name)
        if case.find('skipped') is not None:
            skipped.append(name)
counts = collections.Counter(executed)
result = {
    'source_commit': products['source_commit'],
    'enabled_discovered': len(enabled), 'disabled_discovered': sorted(disabled_names),
    'executions': len(executed), 'distinct_executed': len(counts),
    'passed': len(executed) - len(failed) - len(skipped), 'failed': failed, 'skipped': skipped,
    'missing': sorted(enabled - set(counts)), 'extra': sorted(set(counts) - enabled),
    'duplicates': {name: count for name, count in counts.items() if count != 1},
    'stress_counts': {name: counts[name] for name in expected['stress_counts']}, 'shards': shards,
}
print(json.dumps(result, indent=2))
assert len(discovered) == len(set(discovered))
assert len(shards) == 2 and set(notrun) == disabled_names
assert not failed and not skipped and not result['missing'] and not result['extra'] and not result['duplicates']
assert all(count == 1 for count in result['stress_counts'].values())
assert [shard['xml_sha256'] for shard in shards] == [shard['sha256'] for shard in expected['shards']]
assert result['passed'] == expected['passed'] == result['enabled_discovered']
assert result['source_commit'] == expected['source_commit']
