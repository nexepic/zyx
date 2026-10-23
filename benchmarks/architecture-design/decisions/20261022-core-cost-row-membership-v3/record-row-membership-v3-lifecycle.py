from pathlib import Path
import json,shutil,subprocess
root=Path('/workspace/zyx');d=root/'benchmarks/architecture-design/decisions/20261022-core-cost-row-membership-v3';r=root/'benchmarks/architecture-design/results/20261022-core-cost/row-membership-v3-hybrid'
p=d/'round.json';j=json.loads(p.read_text())
for kind in ['legacy','compact']:
 stem='lifecycle-'+kind+'-';report=json.loads((r/(stem+'tests.json')).read_text());assert report['tests']==253 and report['failures']==0
 j['checks']['hybrid-lifecycle-'+kind]={'tests':253,'failures':0}
 for suffix in ['tests.json','tests.log','receipt.json']:shutil.copy2(r/(stem+suffix),d/(stem+suffix))
for name in ['compile.json','link.json']:shutil.copy2(r/name,d/('lifecycle-'+name))
j['lifecycle_scope']='Regression checks of existing column/extension native paths, plus new low-level membership/page interfaces. This is NOT cold ROW production lifecycle acceptance.'
p.write_text(json.dumps(j,indent=2)+'\n')
p=d/'README.md';s=p.read_text().replace('The experimental module selection passes66 checks and default passes57.','The initial experimental module selection passes66 checks and default passes57. Expanded affected storage lifecycle selections pass253 checks in each of legacy and compact membership configurations. This regression evidence covers existing production column/extension paths; it does not implement the cold ROW production lifecycle.');p.write_text(s)
for lang in ['zh','en']:
 base=root/'docs/apps/docs/content/docs'/lang/'zyx/architecture'
 status='''\n\n当前格式／页接口检查见 `20261022-core-cost-row-membership-v3/`：实验基础 66 项、默认 57 项通过；扩大后的相关生命周期检查在普通及紧凑 membership 配置各 253 项通过。后者证明既有列／扩展路径未回归，不是冷 ROW 生产生命周期完成。未运行全量单测或新性能基准，不增加验收进度。''' if lang=='zh' else '''\n\nCurrent format/page-interface evidence is in `20261022-core-cost-row-membership-v3/`: 66 initial experimental and 57 default checks pass. Expanded affected storage lifecycle selections pass 253 checks in each of legacy and compact membership configurations. This proves regression coverage for existing column/extension paths, not completion of cold ROW production lifecycle integration. No full unit suite or new performance benchmark was run; accepted progress does not increase.'''
 p=base/'native-single-file-storage-plan.mdx';s=p.read_text();assert 'Current format/page-interface evidence' not in s and '当前格式／页接口检查' not in s;p.write_text(s+status+'\n')
 p=base/'optimization.mdx';s=p.read_text();needle='2. **完整成本与转换启动同时证明。**' if lang=='zh' else '2. **Prove whole cost and conversion bootstrap together.**'
 # Keep all remaining tasks as the active queue; attach a concise receipt to item1.
 lines=s.splitlines();idx=next(i for i,l in enumerate(lines) if l.startswith('1. **') and ('ROW remainder' in l));lines[idx]+=(' 当前格式／页接口与既有生命周期回归证据见 `20261022-core-cost-row-membership-v3/`（实验双配置各 253 项、默认 57 项），不等于冷 ROW 生产生命周期完成。' if lang=='zh' else ' Current format/page interfaces and existing-lifecycle regressions are recorded in `20261022-core-cost-row-membership-v3/` (253 experimental checks in each configuration, 57 default checks); this is not cold ROW production lifecycle completion.');p.write_text('\n'.join(lines)+'\n')
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=root))
shutil.copy2(Path('/tmp/record-row-membership-v3-lifecycle.py'),d/'record-row-membership-v3-lifecycle.py')
print('253 legacy +253 compact +57 default checks; accepted progress90%; no timing/commit/push')
