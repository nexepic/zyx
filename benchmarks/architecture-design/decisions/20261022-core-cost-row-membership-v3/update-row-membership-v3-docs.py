from pathlib import Path
root=Path('/workspace/zyx/docs/apps/docs/content/docs')
for lang in ['zh','en']:
 base=root/lang/'zyx/architecture'
 p=base/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('当前组内列位置候选的隔离配置为11／37，明确拒绝此前实验9／35、7／33及6／32文件，不原地迁移。','组内列位置候选已冻结为11／37；当前混合布局候选源码使用13／39，拒绝此前实验11／37、9／35、7／33及6／32文件，不原地迁移。')
  s=s.replace('（当前为下节 11／37）','（组内列位置阶段为下节冻结的 11／37，当前未完成候选为 13／39）')
  s=s.replace('### 当前隔离候选：组成员内的列位置','### 已冻结隔离候选：组成员内的列位置')
  s=s.replace('### 下一核心候选：组内热列与冷 ROW 块（13／39，未集成／未采用）','### 当前核心候选：组内热列与冷 ROW 块（13／39，集成中／未采用）')
  s=s.replace('当前代码仍为 11／37，默认 5／31 不变。','当前实验源码已切换到 13／39，默认 5／31 不变；生产冷 ROW 路由仍显式拒绝，不能当作已接入。')
  s=s.replace('未接入 Native 转换／读写／反向 ownership，也未切换到 13／39；当前持久格式仍为 11／37，进度仍为 90%。','该冻结证据阶段未切换格式。其后的实验源码已切换为 13／39，并接入成员版本 3、同页 group／成员校验、冷块私有页准入和借用读取接口；仍未接入 Native 转换／生产读写／反向 ownership，生产路由显式拒绝新模式，进度仍为 90%。')
 else:
  s=s.replace('The current group-owned locator candidate uses isolated 11/37 and rejects previous experimental 9/35, 7/33 and 6/32 files without in-place migration.','The group-owned locator candidate is frozen at 11/37; current mixed-layout candidate source uses isolated 13/39 and rejects previous experimental 11/37, 9/35, 7/33 and 6/32 files without in-place migration.')
  s=s.replace('(current 11/37 is specified below)','(the frozen 11/37 locator stage and current incomplete 13/39 candidate are specified below)')
  s=s.replace('### Current isolated candidate: column locators','### Frozen isolated candidate: column locators')
  s=s.replace('### Next core candidate: group-owned hot columns and cold ROW blocks (13/39, unintegrated/unadopted)','### Current core candidate: group-owned hot columns and cold ROW blocks (13/39, integration in progress/unadopted)')
  s=s.replace('current code remains 11/37 and default 5/31 is unchanged.','current experimental source has switched to 13/39 and default 5/31 is unchanged. Production cold-ROW routing still refuses explicitly and is not integrated.')
  s=s.replace('Native conversion/read/write/reverse ownership and the 13/39 switch are not integrated. Current persistent format remains 11/37 and progress remains 90%.','That frozen evidence predates the format switch. Subsequent experimental source uses 13/39 with membership version 3, same-page group/membership checks, private cold-block page admission and borrowed reading interfaces. Native conversion/production read/write/reverse ownership remain unintegrated; production routing explicitly refuses the new mode. Progress remains 90%.')
 p.write_text(s)
 p=base/'optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('当前隔离候选 **11／37**','已冻结隔离候选 **11／37**').replace('| 当前 11／37 隔离形状 |','| 冻结 11／37 隔离形状 |')
  s=s.replace('64 个真实形状长 ID 的未来页内几何为 4013 B，但尚未接入真实页／目录／读写，不是采用或性能验收。','64 个真实形状长 ID 的未来页内几何为 4013 B，不是采用或性能验收。当前实验源码已切换至 13／39：成员版本 3、group 同页约束、冷块私有页准入和借用读取接口已加入；Native 转换／生产读写／反向 ownership 尚未接通，生产路由仍显式拒绝新模式。')
 else:
  s=s.replace('isolated candidate **11/37**','frozen isolated candidate **11/37**').replace('| Current isolated 11/37 shape |','| Frozen isolated 11/37 shape |')
  s=s.replace('Future frame geometry for 64 actual-shape long IDs is 4013 B, but no actual page/directory/read/write integration is complete; this is neither adoption nor performance acceptance.','Future frame geometry for 64 actual-shape long IDs is 4013 B, not adoption or performance acceptance. Current experimental source uses 13/39 with membership version 3, same-page group constraints, private cold-block page admission and borrowed reading interfaces. Native conversion/production read/write/reverse ownership remain unintegrated; production routing still refuses the new mode explicitly.')
 p.write_text(s)
 p=base/'native-capability-acceptance-matrix.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('当前隔离候选为37（profile 11；拒绝旧35）','组内列位置候选已冻结为37（profile 11）；当前实验源码为39（profile 13；拒绝旧37／35），冷 ROW 生产生命周期尚未接通且路由仍显式拒绝新模式')
 else:
  s=s.replace('The current isolated candidate is 37 (profile 11; old 35 rejected);','The locator candidate is frozen at 37 (profile 11); current experimental source uses 39 (profile 13; old 37/35 rejected). The cold-ROW production lifecycle remains unintegrated and routing still refuses the new mode explicitly;')
 p.write_text(s)
