from pathlib import Path
import json,subprocess,shutil,hashlib,tarfile
root=Path('/workspace/zyx');d=root/'benchmarks/architecture-design/decisions/20261022-core-cost-cold-row-inspection';d.mkdir(exist_ok=True)
r=root/'benchmarks/architecture-design/results/20261022-core-cost';checks={};libs={}
for kind,stem,build,profile in [('hybrid','selected-','zyx-build-core-hybrid','13/39'),('default','','zyx-build-core-candidate','5/31')]:
 source=r/('cold-row-inspection-'+kind)
 j=json.loads((source/(stem+'tests.json')).read_text());assert j['tests']==21 and j['failures']==0
 checks[kind]={'tests':21,'failures':0}
 for name in [stem+'tests.json',stem+'tests.log',stem+'receipt.json','compile.json','link.json']:
  shutil.copy2(source/name,d/(kind+'-'+name))
 if kind=='hybrid':
  for suffix in ['tests.json','tests.log','receipt.json']:
   shutil.copy2(source/('initial-'+suffix),d/('hybrid-initial-'+suffix))
 target=Path('/workspace')/('zyx-cold-row-inspection-'+kind+'-candidate');target.mkdir(exist_ok=True)
 p=target/'libzyx_core.a';shutil.copy2(Path('/workspace')/build/'libzyx_core.a',p)
 libs[kind]={'path':str(p),'profile':profile,'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest()}
for name in ['build-cold-row-inspection-hybrid-modules.py','build-cold-row-inspection-default-modules.py','run-cold-row-inspection-hybrid-modules.py','run-cold-row-inspection-default-modules.py','record-cold-row-inspection.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
for lang in ['zh','en']:
 base=root/'docs/apps/docs/content/docs'/lang/'zyx/architecture'
 text=('\n\n冷 ROW 根的选读组件见 `20261022-core-cost-cold-row-inspection/`：实验／默认各 21 项必要检查通过。它复用 canonical ROW 与现有 LargeValueStore，按冷块真实 generation 校验整根引用，认证实际 media／manifest／内容哈希及目录长度／字段数，拒绝组内任何热 token，再同步借用选中 canonical value 字节；不构造嵌套 PropertyValue。错误源、未排序请求、取消、额度拒绝、消费者异常与已释放 pin均不输出／重放或泄漏账户。未加入新缓存或提高额度。该组件尚未接入 Native 生产路由／转换／变更／完整 ownership，原路由显式拒绝保留；不是冷 ROW 生命周期或性能验收。\n' if lang=='zh' else '\n\nCold ROW root projection evidence is in `20261022-core-cost-cold-row-inspection/`: 21 necessary checks pass in each experimental/default build. The component reuses canonical ROW and existing LargeValueStore, validates whole-root references against the actual cold-block generation, authenticates actual media/manifest/content hash and directory length/field count, rejects every group-owned hot token, then synchronously borrows selected canonical value bytes without materializing nested PropertyValue trees. Invalid sources, unordered requests, cancellation, admission refusal, consumer exceptions and released pins neither expose/replay invalid output nor leak account ownership. Add no cache or allowance. Native production routing/conversion/mutation/full ownership remain unintegrated and the explicit routing refusal remains. This is not cold ROW lifecycle or performance acceptance.\n')
 p=base/'native-single-file-storage-plan.mdx';s=p.read_text();assert '20261022-core-cost-cold-row-inspection/' not in s;p.write_text(s+text)
 p=base/'optimization.mdx';s=p.read_text();lines=s.splitlines();idx=next(i for i,l in enumerate(lines) if l.startswith('1. **') and 'ROW remainder' in l);lines[idx]+=(' canonical ROW／实际 LargeValue 的认证选读组件已通过双构建各 21 项检查，见 `20261022-core-cost-cold-row-inspection/`；下一步接入同一生产路由，不另做替代载体。' if lang=='zh' else ' The authenticated canonical-ROW/actual-LargeValue projection component passes 21 checks in each build; see `20261022-core-cost-cold-row-inspection/`. Next connect it to the same production route, without an alternative carrier.');p.write_text('\n'.join(lines)+'\n')
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=root))
extra=['include/graph/storage/relocatable/entity/EntityFreeSpaceIndex.hpp','include/graph/storage/relocatable/property/HybridPropertyShortText.hpp','tests/include/storage/relocatable/property/NativeColumnSupplyTestSnapshot.hpp','tests/src/storage/relocatable/property/test_HybridPropertyLayoutScale.cpp','include/graph/storage/relocatable/property/HybridPropertyColdRowBlock.hpp','include/graph/storage/relocatable/property/HybridPropertyColdRowBindings.hpp','src/storage/relocatable/property/HybridPropertyColdRowBlock.cpp','tests/src/storage/relocatable/property/test_HybridPropertyColdRowBlock.cpp','include/graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp','src/storage/relocatable/property/HybridPropertyColdRowInspection.cpp','tests/src/storage/relocatable/property/test_HybridPropertyColdRowInspection.cpp']
with tarfile.open(d/'additional-source.tar.gz','w:gz') as t:
 for name in extra:t.add(root/name,arcname=name)
(d/'round.json').write_text(json.dumps({'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'implemented':['Synchronous canonical ROW root and actual LargeValue selected-byte projection component','Real reference owner/kind/media/block-generation, manifest/content verification and exact actual directory length/count','Group-global hot token rejection before cold source exposure','Selected entry token/canonical framing, NULL/missing and exact scalar bit preservation','Original live snapshot lifetime checks, charged vectors/manifest/range and refusal/cancellation refunds','Consumer failure propagates without replay; no nested PropertyValue construction in source'],'not_implemented':['Native production routing and batch/full/cold source consumption; existing new-mode refusal remains','Native cold conversion/mutation/demotion/delete/inverse/full ownership lifecycle','Cursor one-ROW representation transition and shared-source grouping','Actual mature space/migration bootstrap/final P7 performance matrix'],'scope':'No persisted format change (13/39 and5/31). No new cache/backend/index or allowance. The component is not a producer/publication capability.','performance_claim':None},indent=2)+'\n')
(d/'README.md').write_text('''# Cold canonical ROW and actual LargeValue projection component

Accepted progress remains **90%**; P7 is open. No performance benchmark, full unit suite, commit or push.

The reusable synchronous source now authenticates actual owner-keyed whole-root LargeValue references, media, cold-block generation, manifest/content hash, real directory length/count and group-global hot-token exclusion. Selected entry bytes retain canonical framing and key identity; inline roots borrow the existing decoded cold body. Variable output values remain the consumer's admission responsibility. The source builds no nested PropertyValue tree.

Experimental13/39 and default5/31 each pass21 selected checks (8 inspection and13 existing block/location checks). These include real persisted immutable values with long/nested/NULL fields; authentic hashes containing malformed internal directories/entry tokens still fail; cancellation/admission/source/consumer/pin failures preserve account ownership. Initial20-check evidence is retained separately. Source registration, commands, archive identities and source snapshots accompany this record.

**No production call site consumes this component yet.** The new group mode's explicit production routing refusal remains until the same Native coordinator's reads and lifecycle are integrated. This source component is neither a separate lab carrier nor a publication capability, and provides no latency, mature space or Kuzu claim.
''')
print('21 hybrid +21 default checks; accepted progress90%; source component awaiting production consumption')
