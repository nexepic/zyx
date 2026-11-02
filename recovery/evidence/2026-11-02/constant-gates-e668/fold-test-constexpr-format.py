import pathlib,re
source=pathlib.Path('/workspace/zyx-link-tmp/analyze-constexpr-format.py').read_text().split("for root in ('include','src','tests'):")[0];exec(source)
changed=0;count=0
for p in pathlib.Path('tests').rglob('*.cpp'):
 s=p.read_text();m=mask(s);edits=[]
 for x in pattern.finditer(m):
  start,end=x.start(),x.end();negative=bool(x.group(1));i=end
  if i<len(m) and m[i]=='{':
   depth=1;j=i+1
   while j<len(m) and depth:
    if m[j]=='{':depth+=1
    elif m[j]=='}':depth-=1
    j+=1
   if depth:raise RuntimeError(f'{p}: unbalanced')
   q=j
   while q<len(m) and m[q].isspace():q+=1
   e=re.match(r'else\b\s*',m[q:]);k=None
   if e:
    k=q+e.end()
    if k>=len(m):raise RuntimeError(f'{p}: empty else')
    if m[k]=='{':
     depth=1;l=k+1
     while l<len(m) and depth:
      if m[l]=='{':depth+=1
      elif m[l]=='}':depth-=1
      l+=1
     if depth:raise RuntimeError(f'{p}: unbalanced else')
    else:
     if re.match(r'if\b',m[k:]):raise RuntimeError(f'{p}: else-if requires manual audit')
     depth=0;l=k
     while l<len(m):
      if m[l]=='(':depth+=1
      elif m[l]==')':depth-=1
      elif m[l]==';' and depth==0:l+=1;break
      l+=1
     if l>=len(m):raise RuntimeError(f'{p}: unclosed else statement')
   if negative:
    edits.append((start,k if e else j,''))
   else:
    edits.append((start,end,''))
    if e:edits.append((q,l,''))
  elif negative:
   t=re.match(r'return(?:\s+false)?\s*;',m[i:])
   if not t:raise RuntimeError(f'{p}: unexpected false simple')
   edits.append((start,i+t.end(),''))
  else:edits.append((start,end,''))
  count+=1
 if edits:
  edits.sort()
  for a,b in zip(edits,edits[1:]):
   if a[1]>b[0]:raise RuntimeError(f'{p}: overlapping edits')
  for a,b,replacement in reversed(edits):s=s[:a]+replacement+s[b:]
  p.write_text(s);changed+=1
print('files',changed,'conditions',count)
