import ast, hashlib, json, shutil, subprocess
from pathlib import Path
root=Path('/workspace/zyx'); r=root/'benchmarks/architecture-design/results/20261020-work-resume'
o=root/'benchmarks/architecture-design/decisions/20261021-r32-atomic-vector-publication'
assert not o.exists(); o.mkdir()
base=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
assert base=='e6822035f920b731111bf25b710edf9f19d840f2'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
changed=subprocess.check_output(['git','diff','--name-only'],cwd=root,text=True).splitlines()
manifest={p:sha(root/p) for p in changed}
(o/'source-document-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n')
for p in changed:
 q=o/'snapshot'/p;q.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(root/p,q)
(o/'code.patch').write_bytes(subprocess.check_output(['git','diff','--binary','HEAD','--','include','src','tests'],cwd=root))
(o/'documents.patch').write_bytes(subprocess.check_output(['git','diff','HEAD','--','docs'],cwd=root))
receipts={}
for phase,prefix in [('previous-related','r32-vector-atomic-final'),('current-affected','r32-vector-atomic-affected')]:
 for mode in ['default','candidate']:
  p=json.loads((r/f'{prefix}-{mode}-tests.json').read_text())
  skipped=sum(t.get('result')=='SKIPPED' for suite in p['testsuites'] for t in suite['testsuite'])
  failures=p['failures']; assert not p['errors'] and not p['disabled']
  if phase=='current-affected' or mode=='default':assert failures==0
  else:assert failures==1 and p['tests']==1278
  receipts[f'{phase}-{mode}']={'registered':p['tests'],'passed':p['tests']-skipped-failures,'skipped':skipped,'failed':failures,
   'binary_sha256':sha(r/f'{prefix}-{mode}-tests')}
  for suffix in ['tests.log','tests.json','verified-build.log']:
   name=f'{prefix}-{mode}-{suffix}';shutil.copy2(r/name,o/name)
for name in ['r32-vector-atomic-final-run-module-tests.py','r32-vector-atomic-affected-run-module-tests.py','range-default-final-link.json','range-candidate-link.json',
 'r32-vector-atomic-initial-build.log','r32-vector-atomic-second-build.log','r32-vector-atomic-helper-scope-compile-error.log',
 'r32-vector-atomic-constraint-contract-failure-tests.log','r32-vector-atomic-constraint-contract-failure-tests.json',
 'r32-vector-atomic-signed-zero-before-fix-tests.log','r32-vector-atomic-signed-zero-before-fix-tests.json']:
 shutil.copy2(r/name,o/name)
script=ast.parse((r/'r32-vector-atomic-final-run-module-tests.py').read_text())
modules=next([e.value for e in n.elts] for n in ast.walk(script) if isinstance(n,ast.List) and len(n.elts)==58 and all(isinstance(e,ast.Constant) and isinstance(e.value,str) for e in n.elts))
(o/'modules.json').write_text(json.dumps(modules,indent=2)+'\n')
for lang in ['zh','en']:
 s=(root/f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
 (o/f'{lang}.md').write_text(s[s.rfind('\n### 2026-10-21 R3.2'):])
summary={'base_commit':base,'scope':['R3.2','R6.1'],'progress':'14/24 (58.3%)','accepted_total':14,'denominator':24,'accepted_growth':0,'commit_push':False,
 'related_module_count':58,'tests':receipts,
 'validation_note':'Prior broad run had one LargeValueStore read-fixture failure. Fixture binding and exception expectations were corrected; final signed-zero ROW normalization is verified by current affected runs. The broad candidate run is retained as failed, not recounted as a fresh pass.',
 'excluded':'Unaffected mature multi-group allocation case retains separate frozen evidence.',
 'full_repository_test_run':False,'performance_timing_run':False,'platform':'Linux only',
 'remaining':'R3.2 ordinary native topology/public write routing; production space and complete resource/steady-state gates; remaining R5/R7.'}
(o/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
state=json.loads((r/'continuation.json').read_text());state['latest_checkpoint']=summary|{'evidence':str(o.relative_to(root))}
state['next_implementation']='R3.2 ordinary native topology/public write routing, preserving native owner/field and schema invariants; Vector bootstrap/delta is now explicitly coordinated. R3.3/R5.2/R5.3/R7 remain under fixed document gates.'
state['commit_condition']='Only when accepted progress exceeds14/24; no commit or push in this checkpoint.'
for p in [r/'continuation.json',o/'continuation.json']:p.write_text(json.dumps(state,indent=2)+'\n')
shutil.copy2(Path(__file__),o/Path(__file__).name)
print(json.dumps(summary,ensure_ascii=False))
