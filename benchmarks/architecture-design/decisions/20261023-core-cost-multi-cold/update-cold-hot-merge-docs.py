from pathlib import Path
for lang in ['zh','en']:
 b=Path('docs/apps/docs/content/docs')/lang/'zyx/architecture'
 p=b/'optimization.mdx';s=p.read_text()
 if lang=='zh':s=s.replace('及冷热同批合并，冷字段只重打包受影响块；','及实际写入协调器的冷热同批合并，冷字段只重打包受影响块；',1)
 else:s=s.replace('and merging hot/cold changes in one batch; repack affected blocks only;', 'and wiring shared-image hot/cold preparation into the actual write coordinator; repack affected blocks only;',1)
 p.write_text(s)
 p=b/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s+='\n\n冷热同批共享私有图像的准备已接入既有 `prepareColdBlockWithHot`，证据见 `20261023-core-cost-cold-hot-merge/`。先调用原 DemotionPreparation 构造器，认证所有热计划的数据库、源／目标代次、组与实际列字节；再在同一准备结果上定位已有 COW 图像，缺少的实际源页才 clone，不能为同一页产生另一份从旧快照开始的冷图像。合并继续限制冷热输入合计最多 64 项，不提高原限额，不授予 liveness／全局 token 变更／降级权限。私有目标组／成员矩阵保留热值与 presence 修改，验证冷位置／总数和全局热 tokens／locators 未被热准备改变，再将受影响冷块及最终总数更新到同一图像。使用实际 source 只读接口，原准备器的三个构造器收紧为 const FieldPageStore 引用，不经 const_cast。\n\n14 项定向用例通过，其中一个合并用例分别覆盖热值修改与缺失热 NULL 成员插入后再替换冷行，验证共享页只有一份、冷热修改均保留、成员位图与精确总数一致、文件／根／I/O epoch／ID 不变及账户退款。这仍是私有准备入口；实际 Native 写入分派、块增长拆分／矩阵搬迁、同批新大值、全局降级／owner 生命周期／逆转换、公开发布与完整生命周期及最终性能门禁尚未完成，进度仍 90%。\n'
 else:
  s+='\n\nShared-image hot/cold private preparation now uses existing `prepareColdBlockWithHot`; evidence is in `20261023-core-cost-cold-hot-merge/`. The original DemotionPreparation constructor authenticates hot-plan database/source/target generations, groups and actual column bytes. Cold preparation reuses those COW images, cloning only missing actual source pages; a second cold copy starting from an old snapshot cannot overwrite hot changes. Combined input remains bounded to64 changes with no liveness/global-token/demotion authority. Target group/matrix preserve hot values/presence; cold bindings/counts and global hot tokens/locators must match the original source before affected cold bytes/final totals update in the same image. Three existing constructors now take const FieldPageStore references, reflecting read-only source use without const_cast.\n\n14 targeted cases pass. The merged case separately changes a hot value or inserts a missing-hot NULL member before replacing a cold row, proving one shared page, preserved changes/bitmaps/exact totals, unchanged file/root/I/O epoch/IDs and refunded accounts. This remains private preparation. Actual Native dispatch, block-growth splitting/matrix relocation, batch-local new values, global demotion/owner lifecycle/inverse, public publication/full lifecycle and final performance gates remain pending; progress90%.\n'
 p.write_text(s)
