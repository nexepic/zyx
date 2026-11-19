import importlib.util
from pathlib import Path
import pytest

spec = importlib.util.spec_from_file_location('sort_trace', Path(__file__).parents[1] / 'scripts/check_sort_merge_trace.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample():
    lines = []
    for rows, levels in [(12000, 1), (30000, 2)]:
        decoded, runs = 0, 8
        def emit(phase, level):
            lines.append(f'CI_SORT_MERGE phase={phase} level={level} runs={runs} input_rows={rows} decoded={decoded} '
                         'read_fragments=0 read_payload_bytes=0 written_fragments=0 written_payload_bytes=0 materialized_used=1')
        for level in range(levels):
            emit('begin', level)
            decoded += rows
            runs = 3 if level == levels - 1 else 4
            emit('end', level)
        emit('output-begin', 0)
        decoded += rows
        emit('output-end', 0)
    return '\n'.join(lines)


def test_complete_multi_level_replay():
    assert module.validate(sample()) == {'input_rows': [12000, 30000], 'records': 10}


@pytest.mark.parametrize('old,new', [
    ('phase=output-end', 'phase=failure'), ('level=1', 'level=5'),
    ('input_rows=30000', 'input_rows=5'), ('materialized_used=1', 'materialized_used=-1'),
    ('decoded=24000', 'decoded=24001'), ('read_fragments=0', 'read_fragments=x'),
    ('runs=8', 'runs=3'), ('written_fragments=0', 'written_fragments=999999999999999999999'),
])
def test_incomplete_or_invalid_sort_evidence(old, new):
    with pytest.raises(ValueError):
        module.validate(sample().replace(old, new))


def test_missing_or_truncated_evidence():
    for text in ('', sample() + '\nCI_SORT_MERGE_TRUNCATED process_limit=24'):
        with pytest.raises(ValueError):
            module.validate(text)
