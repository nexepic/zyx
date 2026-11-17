from pathlib import Path
import subprocess,re,json,gzip
root=Path('/workspace/zyx');base='a9e8f5026';head='3d2c8fa98'
files=subprocess.check_output(['git','diff','--name-only',base,head,'--','src','include'],cwd=root,text=True).splitlines()
args=['llvm-cov','export','buildDir/zyx_test_suite','-format=lcov','-instr-profile=buildDir/code.profdata',*files]
lcov=subprocess.check_output(args,cwd=root,text=True)
out=root/'recovery/evidence/2026-11-17/run44091123437'
with gzip.open(out/'changed-production.lcov.gz','wt') as f:f.write(lcov)
counts={};current=None
for line in lcov.splitlines():
 if line.startswith('SF:'):current=str(Path(line[3:]).resolve().relative_to(root));counts.setdefault(current,{})
 elif line.startswith('DA:') and current:
  line_no,hits,*_=line[3:].split(',');counts[current][int(line_no)]=int(hits)
result={'base':subprocess.check_output(['git','rev-parse',base],cwd=root,text=True).strip(),'head':subprocess.check_output(['git','rev-parse',head],cwd=root,text=True).strip(),'scope':'added executable production lines, local unfiltered full-suite profile; not Codecov server patch','files':{}}
for file in files:
 diff=subprocess.check_output(['git','diff','--unified=0',base,head,'--',file],cwd=root,text=True)
 added=[];line_no=0
 for line in diff.splitlines():
  m=re.match(r'@@ -\d+(?:,\d+)? \+(\d+)',line)
  if m:line_no=int(m.group(1));continue
  if line.startswith(('+++','---')):continue
  if line.startswith('+'):
   if line_no in counts.get(file,{}):added.append({'line':line_no,'hits':counts[file][line_no],'source':line[1:]})
   line_no+=1
  elif line.startswith(' '):line_no+=1
 result['files'][file]=added
rows=[r for rs in result['files'].values() for r in rs]
result['added_executable_lines']=len(rows);result['covered_added_executable_lines']=sum(r['hits']>0 for r in rows)
(out/'local-added-line-coverage.json').write_text(json.dumps(result,indent=2)+'\n')
print({k:v for k,v in result.items() if k!='files'})
for file,rs in result['files'].items():
 for r in rs:
  if not r['hits']:print(file,r['line'],r['source'].strip())
