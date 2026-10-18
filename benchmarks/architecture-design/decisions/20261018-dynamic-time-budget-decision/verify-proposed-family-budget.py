"""Check the proposed patch in memory; never change the real runner or deadline."""
from pathlib import Path
import hashlib
import json
import os
import re
import sys
import tempfile
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "benchmarks/graphdb-comparison"))
source = ROOT / "benchmarks/graphdb-comparison/runner/workload_preflight.py"
before = source.read_bytes()
directory = Path(__file__).parent
proof = json.loads((directory / "proposed-family-budget-proof.json").read_text())
assert hashlib.sha256(before).hexdigest() == proof["patch_source_sha256"]
original = before.decode().splitlines(True)
lines = (directory / "proposed-family-budget.patch").read_text().splitlines(True)
result = []
cursor = 0
i = 2
while i < len(lines):
    match = re.match(r"@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@", lines[i])
    assert match, lines[i]
    start = int(match.group(1)) - 1
    result.extend(original[cursor:start])
    cursor = start
    i += 1
    while i < len(lines) and not lines[i].startswith("@@"):
        line = lines[i]
        if line[0] in " -":
            assert original[cursor] == line[1:]
            cursor += 1
        if line[0] in " +":
            result.append(line[1:])
        i += 1
result.extend(original[cursor:])
module = types.ModuleType("proposed_workload_preflight")
module.__file__ = str(source)
exec(compile("".join(result), str(source) + ".proposed", "exec"), module.__dict__)
with tempfile.TemporaryDirectory() as folder:
    for family, profile, configured, elapsed, remaining in [
        ("dynamic", "contract", None, 1799, 1),
        ("dynamic", "contract", "3600", 1800, 1800),
        ("point", "contract", "3600", 1799, 1),
        ("dynamic", "smoke", "3600", 119, 1),
    ]:
        with patch.dict(os.environ, {}, clear=True):
            if configured:
                os.environ["ZYX_APPROVED_DYNAMIC_TIMEOUT_SECONDS"] = configured
            now = [0]
            budget = module.ContractFamilyProgress(Path(folder), (family,), lambda: now[0], profile=profile)
            now[0] = elapsed
            assert budget.remaining() == remaining
            now[0] += remaining
            try:
                budget.remaining()
            except TimeoutError:
                pass
            else:
                raise AssertionError("exact deadline was not enforced")
    with patch.dict(os.environ, {"ZYX_APPROVED_DYNAMIC_TIMEOUT_SECONDS": "3601"}, clear=True):
        try:
            module.ContractFamilyProgress(Path(folder), ("dynamic",)).remaining()
        except ValueError:
            pass
        else:
            raise AssertionError("unregistered deadline accepted")
assert source.read_bytes() == before
print("Five proposed-budget checks passed; real runner and actual deadlines unchanged")
