from pathlib import Path
import json,subprocess
repo=Path('/workspace/zyx');here=repo/'benchmarks/architecture-design/decisions/20261026-s4-native-write-cost';s3=here.parent/'20261026-s3-production-lifecycle';out=repo/'benchmarks/architecture-design/results/20261024-work-scan/s4-native-write-cost'
for name in ['public-b','public-legacy','values-b','generality']:
 r=json.loads((s3/(name+'-recipes.json')).read_text());cmd=r['link'];old=cmd[cmd.index('-o')+1];dest=str(out/(name+'-final'));cmd[cmd.index('-o')+1]=dest
 cmd=[x.replace(old+'-link.d',dest+'-link.d') for x in cmd];subprocess.run(cmd,check=True);(here/(name+'-final-link.json')).write_text(json.dumps(cmd,indent=2)+'\n')
 print(name,'final production relinked',flush=True)
