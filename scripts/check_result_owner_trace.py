#!/usr/bin/env python3
"""Require complete, numeric result-owner records from a bounded native replay."""
import argparse
import json
from pathlib import Path

FIELDS = ('row_before', 'row_after', 'buffered_source_charge', 'used', 'available',
          'cache_nodes', 'cache_edges', 'cache_payload', 'cache_allocator', 'final_result')


def validate(text):
    records = []
    marker = 'CI_RESULT_OWNERSHIP phase=caches_retired '
    for line in text.splitlines():
        if marker not in line:
            continue
        tokens = dict(token.split('=', 1) for token in line.split(marker, 1)[1].split() if '=' in token)
        if any(not tokens.get(field, '').isascii() or not tokens.get(field, '').isdigit() for field in FIELDS):
            raise ValueError('result-owner trace has missing or invalid numeric fields')
        record = {field: int(tokens[field]) for field in FIELDS}
        if record['row_before'] <= record['row_after']:
            raise ValueError('result-owner trace did not retire row cache storage')
        records.append(record)
    if not records:
        raise ValueError('native replay produced no result-owner retirement trace')
    return records


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    records = validate(args.log.read_text(encoding='utf-8', errors='replace'))
    print(json.dumps({'result_owner_records': len(records), 'fields_verified': list(FIELDS)}))
