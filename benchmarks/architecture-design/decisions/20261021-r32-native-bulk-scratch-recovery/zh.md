
### 2026-10-21 R3.2：原生bulk scratch退役与延迟恢复

证据：`benchmarks/architecture-design/decisions/20261021-r32-native-bulk-scratch-recovery/`。原生元数据退役仅在拒绝所有当前公开Page/Extent路由后接受有界BULK_SCRATCH。既有类型化退役批次认证退役身份与旧pin屏障，保留共享池逐帧检查，并避免将bulk scratch误作页池解析。NativeBulkRunStore使用精准释放，避免调用宽泛的原生回收；启动恢复按有界批次清理旧pin留下的已退役bulk，仍有pin时停止复用。

相关7模块：候选279、默认195通过。覆盖孤立bulk可写重开且原生字段/完整性不变；持有旧pin时退役后物理槽仍分配且旧payload可读；释放pin后恢复才释放物理槽。query控制及普通bulk行为继续覆盖。代码快照早于后续原生scratch新分配工作。原生创建/新token/混合/detach/跨组生命周期及生产/R5/R7门槛仍开放。没有全量单测或性能测试。**总计14/24（58.3%），验收增长0，不提交或推送。**
