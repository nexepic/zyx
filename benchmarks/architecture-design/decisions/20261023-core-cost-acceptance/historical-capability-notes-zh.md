原 Native 协调器已接入冷字段分派、冷热合并、受影响块拆分／组目录搬迁以及同批新大值的 Immutable／manifest／content-index 准备，私有候选强制停在首次物理写入前；29 项定向、实验两种 membership 各 507 项相关模块及默认 154 项检查通过。真实 Catalog／索引／约束源覆盖、原子物理安装、全局降级／owner 生命周期／逆转换、完整空间准入和公开生命周期仍未完成，P7／进度仍为 90%。证据：`20261023-core-cost-native-cold-field-coordinator/`。


真实 Catalog／标量索引／NOT NULL 源的协调准备及旧值读取修复见 `20261023-core-cost-native-cold-derived-source/`；30 项定向、实验两种 membership 各 508 项相关模块及默认 154 项通过。恢复修复前旧值读取的单对象对照会误报未修改冷字段缺失；该失败对照单独归档，不计为当前失败或性能结果。公开物理安装、其余 schema 组合及完整生命周期仍未验收，进度仍为 90%。


整列降级的 Native 准备、全部成员 canonical 值证明及原冷目录权限认证见 `20261023-core-cost-cold-global-demotion/`；36 项定向、实验两种 membership 各 514 项相关模块及默认 154 项通过。覆盖 Node／Edge、大值、阈值合并、存活无属性组及非法源／错值拒绝；生产发布和 owner／逆转换完整生命周期仍关闭，P7／进度仍为 90%。

冷逆转换与原有大冷值 writer 内读取：`20261023-core-cost-cold-inverse-coordinator/` 记录原协调器的零写入准备、源 pin 附加修复和失败／退款／重试检查。公开冷模式发布仍关闭，不能计为真实公开转换、owner 生命周期或 P7 通过；进度 90%。

冷 owner 删除／最后组清理与真实唯一约束源：见 `20261023-core-cost-cold-owner-deletion/`。原协调器零写入准备覆盖实际 Entity、字段／存活位／目录及支持树；公开安装、pending／new owner 和 P7 尚未验收，进度 90%。

冷 pending-owner 字段／存活位：`20261023-core-cost-cold-pending-liveness/` 验证实际 63-owner 源上的新冷根、热 NULL、整列降级和 birth/deletion 私有页合并；未接通实际 sealed 实体创建／物理发布，不算公开转换或 P7，进度 90%。

单组 Mode3 实际 Native 发布：`20261023-core-cost-cold-native-lifecycle/` 记录持久化夹具上的冷热修改／降级、创建／删除／清空／最后组／逆转换，以及旧 pin、完整 integrity、实际索引／约束、A/B 重开和取消／退款／重试。通用未适配 guard 与前向公开转换 guard 保留；夹具初始化不算公开转换或空间采用。跨组／共享帧／完整回收搬迁、全库准入与 P7 仍待验收，进度 90%。

共享字段帧实际逆转换／冷大值回收／字段池搬迁：`20261023-core-cost-cold-shared-lifecycle/` 验证不同 namespace 共用帧的保留、原 native 写池合同、旧 pin 阻止回收及排空后的槽／容量释放。仍不是公开前向转换或空间采用；跨组变更批次、全库准入与 P7 未完成，进度 90%。

### 2026-10-23 固定清单收尾验证

实验 13／39 现已支持共享 FieldPage 的跨组／混合 Node、Edge 冷写入，兼容热计划与冷重打包合并到同一私有页集合后一次原子发布；全批次仍受 64 实际变化／迁移 owner 上限。27 项真实持久化生命周期用例包含混合创建／删除、逻辑 API 与两组降级超限拒绝。单帧顺序扫描使用既有有界复制路径，4096 字节缓存的完整过滤查询已由失败变为通过，无缓存额度或校验放宽。实验两种模式各 564 项、默认 154 项及完整查询两种缓存 2 项必要检查通过。

前向冷转换／全目标空间准入仍未交付，公开入口保持关闭；上述夹具不构成转换证明或性能采用。本轮不再展开新迁移协议。最终性能结果、未达条件与源码／库身份见 `benchmarks/architecture-design/decisions/20261023-core-cost-closeout/`；验收仍 90%，不能称 Kuzu 目标完成。
