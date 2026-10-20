"""Validate the uncommitted graph-publication checkpoint, not stage acceptance."""
from pathlib import Path
import hashlib,json,re
here=Path(__file__).resolve().parent
root=here.parents[3]
manifest=json.loads((here/'source-manifest.json').read_text())
assert manifest['source_basis']=='uncommitted working tree' and manifest['code_checkpoint'] is None
assert not manifest['accepted_progress_changed']
for name,digest in manifest['files'].items():
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==digest,name
for profile,count,skipped in [('default',48,0),('candidate',66,1)]:
 result=json.loads((here/(profile+'-results.json')).read_text())
 assert result['tests']==count and result['failures']==0 and result['disabled']==0 and result['errors']==0
 scope=json.loads((here/(profile+'-scope.json')).read_text())
 assert not scope['full_suite'] and len(scope['modules'])==4
 cases=[(suite['name'],case) for suite in result['testsuites'] for case in suite['testsuite']]
 assert len({(suite,case['name']) for suite,case in cases})==count
 skips=[case for _,case in cases if case['result']=='SKIPPED']
 assert len(skips)==skipped
 if skips: assert skips[0]['classname']=='HybridPropertyDescriptorStoreTest' and skips[0]['name']=='DefaultFormatCannotPublishCandidateRoots'
 assert sum(case['result']=='COMPLETED' for _,case in cases)==count-skipped
for lang in ['en','zh']:
 doc=(root/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
 counts=[int(v) for v in re.findall(r'^\| R[0-7].*\| (\d)/3 \|$',doc,re.M)]
 assert len(counts)==8 and sum(counts)==13,(lang,counts)
print('Verified source hashes, default48/48, candidate65/66 plus one default-only skip, and derived13/24')
