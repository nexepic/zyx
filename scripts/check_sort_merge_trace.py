#!/usr/bin/env python3
"""Validate bounded multi-level sort diagnostics from the funded-capacity replay."""
import argparse
import json
from pathlib import Path

FIELDS = ('level', 'runs', 'input_rows', 'decoded', 'read_fragments',
          'read_payload_bytes', 'written_fragments', 'written_payload_bytes', 'materialized_used')
COUNTERS = ('decoded', 'read_fragments', 'read_payload_bytes', 'written_fragments', 'written_payload_bytes')


def validate(text):
    if 'CI_SORT_MERGE_TRUNCATED' in text:
        raise ValueError('sort trace was truncated')
    groups = {}
    for line in text.splitlines():
        if not line.startswith('CI_SORT_MERGE '):
            continue
        tokens = dict(token.split('=', 1) for token in line.split()[1:] if '=' in token)
        if any(not tokens.get(field, '').isascii() or not tokens.get(field, '').isdigit() for field in FIELDS):
            raise ValueError('missing or invalid numeric sort fields')
        record = {field: int(tokens[field]) for field in FIELDS}
        if any(value > 2**63 - 1 for value in record.values()):
            raise ValueError('sort field exceeds bounded integer range')
        record['phase'] = tokens.get('phase')
        groups.setdefault(record['input_rows'], []).append(record)
    if set(groups) != {12000, 30000} or sum(map(len, groups.values())) > 24:
        raise ValueError('expected both bounded replay sizes')
    multi_level = False
    for rows, records in groups.items():
        if [r['phase'] for r in records[-2:]] != ['output-begin', 'output-end']:
            raise ValueError('incomplete final output')
        merges = records[:-2]
        if not merges or len(merges) % 2:
            raise ValueError('incomplete merge levels')
        for level, (begin, end) in enumerate(zip(merges[::2], merges[1::2])):
            if (begin['phase'], end['phase']) != ('begin', 'end') or begin['level'] != level or end['level'] != level:
                raise ValueError('invalid merge phase ordering')
            if not 0 < end['runs'] < begin['runs']:
                raise ValueError('merge did not reduce run count')
            multi_level |= level > 0
        for previous, current in zip(records, records[1:]):
            if any(current[field] < previous[field] for field in COUNTERS):
                raise ValueError('cumulative sort counters regressed')
        begin, end = records[-2:]
        if not 1 <= begin['runs'] <= 3 or end['runs'] != begin['runs'] or end['decoded'] - begin['decoded'] != rows:
            raise ValueError('final stream did not decode every input exactly once')
        if any(end[field] != begin[field] for field in ('written_fragments', 'written_payload_bytes')):
            raise ValueError('final stream unexpectedly rewrote scratch output')
    if not multi_level:
        raise ValueError('multi-level merge was not exercised')
    return {'input_rows': sorted(groups), 'records': sum(map(len, groups.values()))}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    with args.log.open('rb') as stream:
        data = stream.read(2 * 1024 * 1024 + 1)
    if len(data) > 2 * 1024 * 1024:
        raise ValueError('sort replay log exceeds bounded validation size')
    print(json.dumps(validate(data.decode('utf-8', errors='replace'))))
