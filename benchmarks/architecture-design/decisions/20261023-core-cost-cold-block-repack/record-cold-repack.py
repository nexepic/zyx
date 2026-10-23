from pathlib import Path
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx'); d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-block-repack';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,build in [('hybrid','zyx-build-core-hybrid'),('default','zyx-build-core-candidate')]:
 src=r/'benchmarks/architecture-design/results/20261022-core-cost'/('cold-row-codec-'+profile)
 j=json.loads((src/'repack-tests.json').read_text());checks[profile]={k:j.get(k,0) for k in ['tests','failures','skipped']};assert checks[profile]=={'tests':18,'failures':0,'skipped':0}
 for name in ['repack-tests.json','repack-tests.log','repack-link.log','compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
for lang in ['zh','en']:
 p=r/'docs/apps/docs/content/docs'/lang/'zyx/architecture/native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s+='\n\n受影响冷块重打包的字节准备证据见 `20261023-core-cost-cold-block-repack/`。既有 ColdRowBlock codec 新增受限 repack：先认证原块 CRC／owner／目录，再合并最多 64 个完整 owner-root 替换或删除，未改行复制原 canonical 字节；最后一行删除返回空结果，插入缺失 owner 仅是字节准备，不授予 Entity Map liveness 权限。复制、替换或删除前，所有原大值引用均按原块语义代次校验 owner／BYTES／媒体／最小长度，避免升级块代次洗白未来引用；新引用按目标代次检查。实际 manifest／token 热冷排他、源 fence 与原子绑定仍由 Native 协调器认证，codec 不提供发布权限。新块超过原 4 KiB 原文或物理记录边界则整体拒绝，不返回成功前缀；后续需由受影响块布局规划器拆分，不扩大限额。实验／默认配置各 18 项 codec／binding 模块测试通过，含 Node／Edge 未改字节、NULL、删除最后行、缺失 owner 插入、未来引用、重复／非法删除、超限、取消与额度退款。此阶段尚未接入真实变更协调器，不能计作完整冷字段写入或 P7 验收；进度仍 90%，无性能基准／全量单测／提交／推送。\n'
 else:
  s+='\n\nAffected cold-block byte preparation evidence is in `20261023-core-cost-cold-block-repack/`. The existing codec gains bounded repack: authenticate original CRC/owner/directory, then merge at most64 complete owner-root replacements/removals while retaining unchanged canonical bytes. Removing the final row returns empty output; insertion is byte preparation only, not Entity Map liveness authority. Before replacement/deletion, original references retain their original block-generation fence plus owner/BYTES/media/minimum-length checks, preventing generation laundering. New references use the target generation. Actual manifest/token exclusion/source fences and atomic binding remain Native coordinator responsibilities; no publication authority is granted. Exceeding original4KiB/physical-record bounds refuses the whole result without a successful prefix; affected-block placement must split later without raising limits. Experimental/default configurations each pass18 codec/binding module tests covering Node/Edge byte retention, NULL, last-row removal, insertion, future references, duplicates/invalid removals, oversize, cancellation and quota refunds. Actual mutation-coordinator integration remains pending; this is neither complete cold mutation nor P7 acceptance. Progress90%, no benchmark/full suite/commit/push.\n'
 p.write_text(s)
for name in ['build-cold-repack.py','add-cold-repack.py','record-cold-repack.py','build-cold-row-codec-hybrid-modules.py','build-cold-row-codec-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'actual_mutation_integration':False,'remaining':['Affected-block splitting and member matrix/root binding planning','Actual manifest/token exclusion and source fence authentication in mutation coordinator','Atomic publication, deletion/demotion/inverse, full public-conversion lifecycle','Whole-space admission and final P7 performance matrix']},indent=2)+'\n')
print(checks)
