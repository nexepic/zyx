"""Verify this checkpoint's source identities, necessary scope and derived progress."""
from pathlib import Path
import hashlib,json,re
here=Path(__file__).resolve().parent
root=here.parents[3]
manifest=json.loads((here/'source-manifest.json').read_text())
for name,digest in manifest['files'].items():
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
for profile in ['default','candidate']:
 result=json.loads((here/(profile+'-results.json')).read_text())
 assert result['tests']==36 and result['failures']==0 and result['disabled']==0
 scope=json.loads((here/(profile+'-scope.json')).read_text())
 assert not scope['full_suite'] and len(scope['modules'])==4
 cases=[(suite['name'],case['name']) for suite in result['testsuites'] for case in suite['testsuite']]
 assert len(set(cases))==36
for lang in ['en','zh']:
 doc=(root/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
 counts=[int(v) for v in re.findall(r'^\| R[0-7].*\| (\d)/3 \|$',doc,re.M)]
 assert len(counts)==8 and sum(counts)==13,(lang,counts)
print('Verified source hashes, 36/36 necessary cases per configuration, and derived 13/24 progress')
