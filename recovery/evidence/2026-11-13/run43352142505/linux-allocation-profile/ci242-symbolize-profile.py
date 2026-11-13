import csv,json,subprocess,argparse
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--binary',default='/workspace/ci242-rss-profile-baseline')
parser.add_argument('--csv',default='/workspace/ci242-rss-malloc.csv')
parser.add_argument('--output',default='/workspace/ci242-rss-profile-symbols.json')
args=parser.parse_args()
binary=args.binary
rows=list(csv.DictReader(Path(args.csv).read_text().splitlines()[1:]))
for row in rows:
    for key in ('requests','bytes','largest'):row[key]=int(row[key])
selected=[]
for key in ('requests','bytes'):
    for row in sorted(rows,key=lambda row:row[key],reverse=True)[:12]:
        if row not in selected:selected.append(row)
for row in rows:
    if row['largest'] in (130960,131008) and row not in selected:selected.append(row)
addresses=sorted({row['p'+str(i)] for row in selected for i in range(8)}-{'0'})
p=subprocess.run(['llvm-symbolizer-19','--obj='+binary,'--demangle','--inlining=false','--output-style=JSON'],
    input='\n'.join(addresses)+'\n',text=True,capture_output=True,timeout=120,check=True)
symbols={}
for address,line in zip(addresses,p.stdout.splitlines()):
    value=json.loads(line)
    symbols[address]=value
for row in selected:
    row['stack']=[symbols.get(row['p'+str(i)]) for i in range(8) if row['p'+str(i)]!='0']
Path(args.output).write_text(json.dumps(dict(
    requested_bytes=sum(row['bytes'] for row in rows),requests=sum(row['requests'] for row in rows),
    stack_buckets=len(rows),profile='Linux cumulative requests after query start; includes completion/cleanup; no native RSS attribution',
    selected=selected),indent=2)+'\n')
for row in sorted(selected,key=lambda row:row['bytes'],reverse=True)[:14]:
    names=[]
    for item in row['stack']:
        syms=item.get('Symbol',[])
        for sym in syms:
            names.append(dict(function=sym.get('FunctionName','')[:180],file=Path(sym.get('FileName','')).name,line=sym.get('Line')))
    print(json.dumps(dict(requests=row['requests'],bytes=row['bytes'],frames=names[-4:])))
