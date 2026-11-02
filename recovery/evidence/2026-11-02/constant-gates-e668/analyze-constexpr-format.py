import pathlib,re
pattern=re.compile(r'\bif\s+constexpr\s*\(\s*(!?)\s*RSS_(?:HYBRID_PROPERTY|SCAN_PATH)_FORMAT_ENABLED\s*\)\s*')
def mask(s):
 out=list(s);i=0;n=len(s)
 while i<n:
  if s.startswith('//',i):
   j=s.find('\n',i);j=n if j<0 else j
  elif s.startswith('/*',i):
   j=s.find('*/',i+2);j=n if j<0 else j+2
  elif s[i] in '\"\'':
   q=s[i];j=i+1
   while j<n:
    if s[j]=='\\':j+=2;continue
    if s[j]==q:j+=1;break
    j+=1
  else:i+=1;continue
  for k in range(i,j):
   if s[k]!='\n':out[k]=' '
  i=j
 return ''.join(out)
for root in ('include','src','tests'):
 for p in pathlib.Path(root).rglob('*'):
  if not p.is_file() or p.suffix not in ('.hpp','.cpp','.inc','.h','.cc'):continue
  s=p.read_text();m=mask(s)
  for x in pattern.finditer(m):
   i=x.end();line=s.count('\n',0,x.start())+1;kind='simple';els=''
   if i<len(m) and m[i]=='{':
    depth=1;j=i+1
    while j<len(m) and depth:
     if m[j]=='{':depth+=1
     elif m[j]=='}':depth-=1
     j+=1
    if depth:raise RuntimeError(f'unmatched {p}:{line}')
    q=j
    while q<len(m) and m[q].isspace():q+=1
    els=' ELSE' if re.match(r'else\b',m[q:]) else ''
    kind='block'
   print(f'{p}:{line} {"false" if x.group(1) else "true"} {kind}{els}')
