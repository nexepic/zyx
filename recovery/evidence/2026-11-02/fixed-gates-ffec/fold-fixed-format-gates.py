import pathlib,re,sys
true_expr={'ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT','ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT'}
false_expr={'!ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT','!ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT'}
changed=[]; removed=0
for root in ('include','src','tests'):
 for p in pathlib.Path(root).rglob('*'):
  if not p.is_file() or p.suffix not in ('.hpp','.cpp','.h','.cc'): continue
  lines=p.read_text().splitlines(keepends=True); out=[]; stack=[]; active=True; count=0
  for line in lines:
   m=re.match(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)',line)
   if not m:
    if active: out.append(line)
    continue
   kind,arg=m.group(1),m.group(2).strip()
   if kind in ('if','ifdef','ifndef'):
    val=(arg in true_expr) if kind=='if' and arg in true_expr|false_expr else None
    stack.append({'parent':active,'known':val is not None,'val':val,'else':False})
    if val is None:
     if active:out.append(line)
    else:
     count+=1;active=active and val
   elif kind=='elif':
    if not stack: raise RuntimeError(f'{p}: orphan elif')
    if stack[-1]['known']:raise RuntimeError(f'{p}: target block has elif; manual audit required')
    if stack[-1]['parent']:out.append(line)
    active=stack[-1]['parent']
   elif kind=='else':
    if not stack:raise RuntimeError(f'{p}: orphan else')
    f=stack[-1]
    if f['known']:
     if f['else']:raise RuntimeError(f'{p}: duplicate else')
     f['else']=True;active=f['parent'] and not f['val']
    else:
     if f['parent']:out.append(line)
     active=f['parent']
   else:
    if not stack:raise RuntimeError(f'{p}: orphan endif')
    f=stack.pop();active=f['parent']
    if not f['known'] and active:out.append(line)
  if stack:raise RuntimeError(f'{p}: unclosed condition')
  text=''.join(out)
  if text!=''.join(lines):p.write_text(text);changed.append(str(p));removed+=count
print('files',len(changed),'folded',removed)
