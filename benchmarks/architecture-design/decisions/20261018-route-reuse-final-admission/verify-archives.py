#!/usr/bin/env python3
"""Verify preserved evidence without extracting files or running benchmarks."""
from pathlib import Path
import hashlib,json,tarfile,tempfile
root=Path(__file__).resolve().parent
for name in ['archive-manifest.json','performance-archive-manifest.json']:
 manifest=json.loads((root/name).read_text())
 with tempfile.TemporaryFile() as stream:
  whole=hashlib.sha256();size=0
  for part in manifest.get('parts',[{'path':manifest['archive'],'bytes':manifest['archive_bytes'],'sha256':manifest['archive_sha256']}]):
   digest=hashlib.sha256();count=0
   with (root/part['path']).open('rb') as source:
    for block in iter(lambda:source.read(1048576),b''):
     digest.update(block);whole.update(block);count+=len(block);stream.write(block)
   assert count==part['bytes'] and digest.hexdigest()==part['sha256'],part['path'];size+=count
  assert size==manifest['archive_bytes'] and whole.hexdigest()==manifest['archive_sha256']
  stream.seek(0);expected={v['path']:v for v in manifest['records']};seen=set()
  with tarfile.open(fileobj=stream,mode='r|gz') as archive:
   for member in archive:
    assert member.isfile() and member.name in expected and member.name not in seen
    digest=hashlib.sha256();count=0
    with archive.extractfile(member) as source:
     for block in iter(lambda:source.read(1048576),b''):digest.update(block);count+=len(block)
    entry=expected[member.name];assert count==entry['bytes'] and digest.hexdigest()==entry['sha256'],member.name;seen.add(member.name)
  assert seen==set(expected)
 print(f'{name}: {len(seen)} members verified')
