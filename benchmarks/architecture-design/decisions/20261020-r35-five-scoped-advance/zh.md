# R3/R5 固定五项推进检查点

本轮只推进 R3.2、R3.3、R5.1、R5.2、R5.3。它们仍未阶段验收；总进度 13/24（54.2%），没有增长，因此没有提交或推送。

- R3.2：在同一 MetadataWriteBatch 内准备独立字段 LargeValue 的真实数据/manifest 私有帧、连续逻辑页和共享物理池。编码工作区及保留图像计费，取消/额度拒绝退款；不写文件。池字节计数现包含这些大值池，仍不是完整发布成本。
- R3.3：NativeGraphWriteTransaction 内部可暂存有账户的转换请求，保留源 pin、精确源 fence 和账户所有权；禁止混入普通写/ID 租约。移动/回滚释放正确；新发布使请求冲突。提交目前仍遇到未安装物理发布保护，按既有异常合同进入 INDETERMINATE，文件和根不变。这不是成功转换，也不是 backend/公共 API 全链路。
- R5.1：原生全局邻接 continuation 绑定数据库身份、generation、方向和节点，两个 adapter 翻译点一并传递身份。保留分页重入测试。更高层游标完整跨 pin 合同、覆盖/分段原生生命周期仍未完成。
- R5.2：多节点私有邻接批先校验全部主 Edge，再缓存/调用消费者；后部不一致不能被早停或过滤隐藏。主 Edge 认证继续保留，没有采用新布局。
- R5.3：现有锚点成本重排仅穿过已证明透明的输出层；LIMIT、SKIP、OPTIONAL、DISTINCT、易变/属性表达式及未知层保留原方向。新增候选路径成本与留出负载验收仍未完成。

六个受影响测试模块（包含分页 adapter）在默认构建 121/121、实验格式构建 135/135 通过，原始输出见 raw，源码身份见 source-manifest.json。只验证 Linux；未执行全量单测或新性能测试。初始基准沿用已冻结的 20261020-work-resume/start，最后统一测试窗口尚未开始。编译修正记录保留，不能当作性能证据。

后续仍须完成物理池/目录/描述符原子发布、图属性读写/overlay/索引路由、双向转换和删除、恢复/完整性/退休/搬迁/复用、完整真实分配成本及同执行器 ROW 对照；R5 候选同样必须完成原子双向生命周期和成本验证。未满足之前保持保护，不能通过删除保护宣称完成。

## R3.2 后续：单字段降级私有协调

新增 HybridPropertyDemotionPreparation：同一 MetadataWriteBatch 的精确来源 fence 下，先认证 group/column 原记录，再按真实来源页合并复制，更新 group 计数并移除旧 column slot；只为该字段生成最多 64 个 extension、owner 来源切换/删除和最终 locator。extension 共享页面，无关 extension（含独立大值引用）保持字节不变。超大新值必须经同批次独立字段 resolver，不能嵌入整行载体；没有 resolver 时明确拒绝。源 ID、文件及根不变，batch 私有 ID 随失败整批丢弃。

测试覆盖类型变化、字段移除、NULL、实际 locator/值、无关字节、源旧列、1 KiB 拒绝、私有帧复制后取消、私有 ID 分配后取消及独立大字段 resolver。整 owner 删除继续拒绝，因为必须同时协调该 owner 的全部字段/live 位；此处不能只改一个列。多个并发字段变更的最终 map 合并、物理 COW 登记/退役及原子发布仍须完成，不能将此检查点计作完整生命周期或 R3.2/R3.3 验收。

最新降级检查点受影响模块通过默认 4/4、实验 45/45 项测试。初次测试调用遗漏 admission 回调、超大 sibling 夹具误选 inline 模式的失败与修正均保留在 raw；最终结果与源码身份见 demotion-proof.json / demotion-source-manifest.json。
