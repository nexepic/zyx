import importlib.util
from pathlib import Path
import pytest

spec = importlib.util.spec_from_file_location('owner_trace', Path(__file__).resolve().parents[1] / 'scripts/check_result_owner_trace.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

RECORD = ('CI_RESULT_OWNERSHIP phase=caches_retired row_before=624 row_after=344 '
          'buffered_source_charge=184 used=3105 available=1239 cache_nodes=2 cache_edges=0 '
          'cache_payload=720 cache_allocator=320 final_result=1145 reports_limit=20')


def test_preserves_independent_owner_fields_without_summing_overlaps():
    values = module.validate('irrelevant\n2026-11-17T08:00:00Z ' + RECORD + '\n')
    assert len(values) == 1
    assert values[0]['cache_payload'] == 720 and values[0]['final_result'] == 1145


@pytest.mark.parametrize('text', ['', RECORD.replace('cache_payload=720 ', ''),
                                  RECORD.replace('cache_allocator=320', 'cache_allocator=-1'),
                                  RECORD.replace('row_after=344', 'row_after=624')])
def test_missing_or_malformed_evidence_is_a_failure(text):
    with pytest.raises(ValueError):
        module.validate(text)
