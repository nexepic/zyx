
### 2026-10-21 R3.2：公共同类原生所有者删除

证据：`benchmarks/architecture-design/decisions/20261021-r32-public-native-deletion/`。公共孤立Node及Edge删除选择已认证原生所有者，复用字段/Entity/邻接/标量/约束/计数/Vector生命周期写入器，共同发布。同类原生批次保留64所有者与1MiB限制；ROW/原生混合删除和detach仍受保护，有关联边的Node在发布前拒绝。

相关5模块：候选189、默认111通过。新增公共用例覆盖旧快照/重开、孤立Node删除、Edge邻接/计数、Vector成员删除且保留旧embedding、完整64条Edge组删除及完整性/退款、有边Node/未支持detach拒绝且字段不变。源代码快照早于后续patch接入。原生创建、patch与普通混合/detach生命周期、生产空间/资源及R5/R7仍未完成。没有全量单测或性能测试。**总计14/24（58.3%），验收增长0，不提交或推送。**
