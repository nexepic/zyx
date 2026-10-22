"""Rebuild only the observer programs using the recorded build environment.

Run before timing. The engine archive must already match freeze.json. The recorded
commands identify include and library paths; port them explicitly for other hosts.
"""
from pathlib import Path
import hashlib,json,subprocess
HERE=Path(__file__).resolve().parent
freeze=json.loads((HERE/'freeze.json').read_text())
recipe=json.loads((HERE/'build.json').read_text())
for name,build in recipe.items():
 archive=Path(build['cwd'])/'libzyx_core.a'
 assert hashlib.file_digest(archive.open('rb'),'sha256').hexdigest()==freeze['library_sha256']
 log=Path(build['binary']).with_suffix('.rebuild.log')
 with log.open('w') as out:
  for command in [build['compile'],build['link']]:subprocess.run(command,cwd=build['cwd'],stdout=out,stderr=subprocess.STDOUT,check=True)
 print(name,hashlib.file_digest(Path(build['binary']).open('rb'),'sha256').hexdigest())
