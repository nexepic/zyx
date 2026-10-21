
### 2026-10-21 R3.2：原生布局下回收孤立query scratch

证据：`benchmarks/architecture-design/decisions/20261021-r32-native-query-scratch-recovery/`。用例复现了原生组与已封存孤立query scratch并存时可写打开失败。现有query专用释放在认证有界类型/所有者/代次/分配身份并拒绝所有公开Page/Extent引用后进入类型化原生写入端，在同次元数据发布中迁移未修改描述符。保留私有读者静默及envelope/校验和验证。

相关7模块：候选277、默认195通过，涵盖启动回收且原生查询结果不变与完整性、query/bulk类型隔离、外来代次/所有者/重复身份及损坏公开Page/Extent注册拒绝。保留原始失败及后续bulk修改前的准确源码。bulk scratch退役/恢复继续单独推进，其余原生写入、生产/R5/R7缺口仍开放。没有全量单测或性能测试。**总计14/24（58.3%），验收增长0，不提交或推送。**
