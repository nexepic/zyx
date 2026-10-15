# overlay 与 undo 节点归属

新增可选 LogicalGraphTransactionMemoryProvider 提供共享 WRITE owner，不改变现有事务接口的虚函数布局；原生事务实现该能力。overlay 在 PMR 映射之前保留 owner，节点/边状态节点、持久可见性节点、语句 undo 节点、patch 目标校验及 detach 去重节点使用该额度；其他后端维持永久 new/delete 分配行为。Node/Edge 内嵌字符串、向量和 PropertyValue 仍未包含在本次元数据接入中。

测试耗尽共享 WRITE 准入，验证 undo 拒绝后保留前一条可见及持久语句，创建 undo 拒绝后可以相同预览身份重试。原生回滚后仍存活的 overlay 持续计费，销毁后归还。既有语句回滚仍通过交换已保存 owner 完成，不增加分配。

当前源码选定的 53 个套件、1,304 项测试全部通过，覆盖通用 overlay 语义、undo、Vector 与精确候选路径。独立进程另遍历 overlay 暂存正常轨迹的 17 个分配点，验证可见状态与重开后完整属性，并通过 4 项既有 OOM/保留测试。生产和基准程序没有替换全局 new/delete。测试入口新增 overlay/all 阶段选择，原有 stage/commit 选择保持兼容。

后续审计发现 const 暂存结果在禁用 NRVO 时可能返回复制；本检查点先于该路径的修复和验证，不代表最终源码验收。其他 WRITE/私有 COW/嵌套负载、维护/积压及分区算子仍待完成。没有新增固定验收：22/34（64.7%）。没有执行全量单元测试或性能测试。
