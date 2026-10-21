
### 2026-10-21 R3.2：原生字段激活时分配私有scratch

证据：`benchmarks/architecture-design/decisions/20261021-r32-native-scratch-allocation/`。用例复现了原生转换后scratch创建被拒绝。纯Segment-ID命名空间预留现以64ID为界保持原生字段；物理分配仅允许既有preparer写入最多64个新建、封存、同类、同所有者的QUERY/BULK scratch，禁止公开Page/Extent/Entity/Vector路由且要求身份不存在。两次发布均迁移未修改原生描述符，普通未适配池写入仍受保护。

主要7模块：默认195、候选281通过，覆盖新query/bulk scratch分配/清理、源查询/完整性、旧pin退役及Segment命名空间两种A/B中断边界。额外4个分配/元数据模块：默认59通过；候选49通过、10个物理分配器故障/回收用例因池envelope与Catalog不一致失败。只将两个元数据归档对象替换为HEAD实现后复现完全相同的10项失败；该对照保留当前头文件和其余归档对象，并非完整HEAD检出。归档所有失败、对照源码与日志，不将候选控制组记为全绿。命名空间故障用例已包含在281中，不重复计数。源码快照早于测试fixture拆分。原生所有者插入/新token/混合/detach/跨组及完整生产/资源/稳态/R5/R7仍开放。没有全量单测或性能测试。**总计14/24（58.3%），验收增长0，不提交或推送。**
