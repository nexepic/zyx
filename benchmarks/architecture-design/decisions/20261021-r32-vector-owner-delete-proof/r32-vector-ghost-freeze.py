import ast, hashlib, json, shutil, subprocess
from pathlib import Path
root=Path('/workspace/zyx'); r=root/'benchmarks/architecture-design/results/20261020-work-resume'
out=root/'benchmarks/architecture-design/decisions/20261021-r32-vector-owner-delete-proof'
assert not out.exists()
out.mkdir()
base=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
assert base=='e6822035f920b731111bf25b710edf9f19d840f2'
sha=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
changed=subprocess.check_output(['git','diff','--name-only'],cwd=root,text=True).splitlines()
manifest={p:sha(root/p) for p in changed}
(out/'source-document-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
(out/'code.patch').write_bytes(subprocess.check_output(['git','diff','--binary','HEAD','--','include','src','tests'],cwd=root))
(out/'documents.patch').write_bytes(subprocess.check_output(['git','diff','HEAD','--','docs'],cwd=root))
for p in changed:
 target=out/'snapshot'/p; target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(root/p,target)
receipts={}
for phase,prefix in [('related','r32-vector'),('final-affected','r32-vector-final')]:
 for mode in ['default','candidate']:
  p=json.loads((r/f'{prefix}-{mode}-tests.json').read_text())
  assert p['failures']==0 and p['disabled']==0
  skipped=sum(t.get('result')=='SKIPPED' for suite in p['testsuites'] for t in suite['testsuite'])
  receipt={'registered':p['tests'],'passed':p['tests']-skipped,'skipped':skipped,
           'binary_sha256':sha(r/f'{prefix}-{mode}-tests')}
  receipts[f'{phase}-{mode}']=receipt
  for suffix in ['tests.log','tests.json','verified-build.log']:
   name=f'{prefix}-{mode}-{suffix}';shutil.copy2(r/name,out/name)
for name in ['r32-vector-run-module-tests.py','r32-vector-final-run-module-tests.py','range-default-final-link.json','range-candidate-link.json']:
 shutil.copy2(r/name,out/name)
script=ast.parse((r/'r32-vector-run-module-tests.py').read_text())
modules=next([e.value for e in n.elts] for n in ast.walk(script) if isinstance(n,ast.List) and len(n.elts)==57 and all(isinstance(e,ast.Constant) and isinstance(e.value,str) for e in n.elts))
(out/'modules.json').write_text(json.dumps(modules,indent=2)+'\n')
summary={'base_commit':base,'scope':['R3.2'],'accepted_total':14,'denominator':24,'percentage':58.3,'accepted_growth':0,'commit_push':False,
 'related_module_count':len(modules),'tests':receipts,
 'validation_note':'Related-module runs preceded the dimension guard and four subsequent dependency/deletion cases; final-affected runs rebuilt the current source and exercised these changes. Broad and final binaries have separate hashes.',
 'excluded':'Unaffected HybridPropertyMatureShapeCostsTest has separately frozen evidence.',
 'performance_timing_run':False,'full_repository_test_run':False,
 'remaining':'Actual Vector delta publication and ordinary topology integration; production space/resource gates and remaining R5/R7.'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for language in ['zh','en']:
 text=(root/f'docs/apps/docs/content/docs/{language}/zyx/architecture/optimization.mdx').read_text()
 (out/f'{language}.md').write_text(text[text.rfind('\n### 2026-10-21 R3.2'):])
state=json.loads((r/'continuation.json').read_text())
state['latest_checkpoint']=summary|{'evidence':str(out.relative_to(root))}
state['next_implementation']='R3.2 atomic Vector delta publication and ordinary native topology integration, then R3.3/R5.2/R5.3/R7 under the fixed document contracts.'
state['commit_condition']='Only when accepted progress exceeds14/24; no commit or push for this checkpoint.'
(r/'continuation.json').write_text(json.dumps(state,indent=2)+'\n')
(out/'continuation.json').write_text(json.dumps(state,indent=2)+'\n')
print(json.dumps(summary,ensure_ascii=False))
