from pathlib import Path
import hashlib,json,re
root=Path(__file__).resolve().parents[4]
d=Path(__file__).resolve().parent
proof=json.loads((d/'proof.json').read_text())
for name,sha in json.loads((d/'source-manifest.json').read_text()).items():
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==sha,name
counts=[]
for lang in ['zh','en']:
 text=(root/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
 stages=[int(v) for v in re.findall(r'^\| R[0-7].*\| (\d)/3 \|$',text,re.M)]
 assert len(stages)==8
 counts.append((sum(stages),len(stages)*3))
assert counts[0]==counts[1]==(proof['accepted_top_level_items'],proof['total'])
def cases(path):
 result=json.loads(path.read_text());assert result['failures']==0,path
 return {(suite['name'],test['name']):test for suite in result['testsuites'] for test in suite['testsuite']}
tokens=[]
for profile in ['candidate','default']:
 main=cases(d/(profile+'-lifecycle-final.json'))
 targeted=cases(d/(profile+'-root-profile-final.json'))
 main.update(targeted)
 stats=proof['validation'][profile]
 assert len(main)==stats['unique_latest_cases']
 assert sum(t['result']=='SKIPPED' for t in main.values())==stats['skipped']
 token=targeted[('HybridPropertyDescriptorStoreTest','FenceIncludesDescriptorEvenWhenOtherGraphRootsAreUnchanged')]
 tokens.append(token['graph_fence_format_token'])
 assert token['format_profile']==str(stats['format_profile'])
assert tokens[0]!=tokens[1]
print('Source hashes, profile identity, selected tests and bilingual progress verified')
