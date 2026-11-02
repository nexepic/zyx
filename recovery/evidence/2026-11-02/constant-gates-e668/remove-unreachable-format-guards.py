import pathlib,re
source=pathlib.Path('/workspace/zyx-link-tmp/analyze-constexpr-format.py').read_text().split("for root in ('include','src','tests'):")[0];exec(source)
pat=re.compile(r'(?m)^[ \t]*if\s*\(\s*!RSS_(?:HYBRID_PROPERTY|SCAN_PATH)_FORMAT_ENABLED\s*\)\s*')
total=0;files=0
for root in ('include','src'):
 for p in pathlib.Path(root).rglob('*'):
  if not p.is_file() or p.suffix not in ('.hpp','.cpp','.inc','.h','.cc'):continue
  s=p.read_text();m=mask(s);edits=[]
  for x in pat.finditer(m):
   i=x.end(); t=re.match(r'(?:throw|return|fail)\b',m[i:])
   if not t:raise RuntimeError(f'{p}: unexpected statement')
   j=i;parens=braces=0
   while j<len(m):
    c=m[j]
    if c=='(':parens+=1
    elif c==')':parens-=1
    elif c=='{':braces+=1
    elif c=='}':braces-=1
    elif c==';' and parens==0 and braces==0:j+=1;break
    j+=1
   if j>=len(m):raise RuntimeError(f'{p}: incomplete statement')
   q=j
   while q<len(m) and m[q].isspace():q+=1
   if re.match(r'else\b',m[q:]):raise RuntimeError(f'{p}: else requires manual audit')
   if j<len(s) and s[j]=='\n':j+=1
   edits.append((x.start(),j));total+=1
  if edits:
   for a,b in reversed(edits):s=s[:a]+s[b:]
   p.write_text(s);files+=1
print('files',files,'guards',total)
