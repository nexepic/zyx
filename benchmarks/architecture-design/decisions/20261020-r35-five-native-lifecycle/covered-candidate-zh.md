# R5.1 覆盖树原生候选合同

仅实验候选，不安装自动图路由、不保留 IndexId、不授予生产准入。复用原生 EMPTY 固定 map 叶：17字节保序 (方向, NodeId, EdgeId) 逻辑键，后接16字节小端 (NeighborId, TypeId)。同逻辑前缀重复即损坏，不能由不同 suffix 隐藏。TypeId 允许0；节点/边/邻居身份必须为正。调用方实验 index 不能别名生产全局邻接或保留命名空间。

原生图事务将两方向记录与权威 Edge 一起暂存，保留自环和多重边。读取绑定实际 Index Root Directory 根、数据库和来源 pin；从 after-EdgeId+1 有界 seek 续读，不从头重扫超级节点。最多64个输出与一个已认证 lookahead，在全部主 Edge 检查完成前保持私有。continuation 绑定数据库/代数/方向/节点/last/精确next。主记录认证不删除；复用原生 allocator、COW 退役与 A/B fence。

这只是一个候选的协议增量。全局反向项完整性、backend type索引/统计/overlay 协调、分段候选生命周期、搬迁/复用、完整成本和最终留出性能准入仍须完成。合法 checksum 的缺失/多余/错误方向记录仍需完整 integrity 核对，不能由点读证明全局完整性。此合同不授予阶段验收或性能收益。
