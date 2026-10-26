"""Restore byte-verified historical source evidence into an explicit workspace."""
from pathlib import Path
import hashlib,json,sys,tarfile
if len(sys.argv)!=2:raise SystemExit('usage: restore.py HISTORICAL_WORKSPACE')
here=Path(__file__).resolve().parent;dest=Path(sys.argv[1]).resolve()
with tarfile.open(here/'sources.tar.gz') as archive:
 for item in json.loads((here/'manifest.json').read_text()):
  target=(dest/item['path']).resolve()
  if not target.is_relative_to(dest):raise ValueError('invalid archive path')
  raw=archive.extractfile(item['path']).read()
  if hashlib.sha256(raw).hexdigest()!=item['sha256']:raise ValueError('evidence hash mismatch')
  if target.exists() and target.read_bytes()!=raw:raise FileExistsError(target)
  target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
