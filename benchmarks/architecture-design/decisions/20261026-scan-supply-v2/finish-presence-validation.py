"""Targeted module validation after the isolated lifecycle run completes."""
from pathlib import Path
import hashlib
import json
import subprocess
import time

D = Path(__file__).resolve().parent
R = D.parents[3]
deadline = time.monotonic() + 2000
while not (D / 'generality-presence-receipt.json').exists():
    assert time.monotonic() < deadline, 'Lifecycle receipt did not arrive'
    time.sleep(1)
assert json.loads((D / 'generality-presence-receipt.json').read_text())['returncode'] == 0

def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()

receipts = []
for profile, binary, library, count, recipe in [
    ('scan', D / 'presence-scan-modules', R / 'build-scan-candidate/libzyx_core.a', 91, None),
    ('hybrid', D / 'hybrid/modules', R / 'build-hybrid-compat/libzyx_core.a', 102, D / 'hybrid/recipes.json'),
]:
    before = sha(library)
    if recipe:
        with (D / 'presence-hybrid-link.log').open('w') as output:
            subprocess.run(json.loads(recipe.read_text())[-1], stdout=output, stderr=output, check=True)
    output = D / ('presence-' + profile + '-modules.json')
    assert not output.exists(), 'Preserve earlier validation receipts'
    command = [str(binary), '--gtest_output=json:' + str(output)]
    print('START', profile, flush=True)
    with output.with_suffix('.log').open('w') as log:
        subprocess.run(command, stdout=log, stderr=log, check=True, timeout=1200)
    result = json.loads(output.read_text())
    assert result['tests'] == count and result['failures'] == 0, result
    assert sha(library) == before, 'Library changed during tests'
    receipts.append(dict(profile=profile, tests=count, failures=0, command=command,
                         library_sha256=before, binary_sha256=sha(binary)))
    print('PASS', profile, count, flush=True)
(D / 'presence-module-receipt.json').write_text(json.dumps(receipts, indent=2) + '\n')
