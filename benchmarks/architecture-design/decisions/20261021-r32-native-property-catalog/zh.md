
### 2026-10-21 R3.2：新属性token与原生字段及Vector共同发布

证据：`benchmarks/architecture-design/decisions/20261021-r32-native-property-catalog/`。公共原生map/patch可在同源临时Catalog事务中分配新属性名，仅将规范的属性正反映射及属性序列转交字段事务，排除Schema/索引/约束控制修改。绑定源及实际derived对象的私有证明将Catalog根纳入既有字段/Vector批次；只有该密封Catalog转换才允许空Vector语义。临时事务不发布。暂存名称在分配前准入、信用转移前实际销毁，标量缓冲/闭包销毁后再退款；保留原1MiB账户，不声称完整RSS准入。

相关6模块：候选203、默认111通过。覆盖整体替换及选择性patch新名称、旧快照与删除/恢复、64所有者在原预算内共享新token、新名称/Vector联合变更、非法Vector值及暂存后约束拒绝不留下Catalog名称、部分/完整bootstrap写入中断后Catalog/字段/Vector共同恢复与完整性。初版拒绝用例误期望FormatException，既有Vector接口实际正确抛出invalid_argument，保留原始日志。源码快照包含已拆分共享fixture和公共生命周期模块。原生所有者插入/混合/detach/跨组生命周期、底层原语token成员校验及完整资源/空间/稳态/R5/R7仍开放。没有全量单测或性能测试。**总计14/24（58.3%），验收增长0，不提交或推送。**
