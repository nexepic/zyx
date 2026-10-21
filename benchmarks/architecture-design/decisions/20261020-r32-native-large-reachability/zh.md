### 2026-10-20 R3.2：混合所有权扫描与凭据保护的原生大值回收

证据：`benchmarks/architecture-design/decisions/20261020-r32-native-large-reachability/`。直接有界回收现合并 ROW Property Directory、Node／Edge 原生成员的逻辑顺序；通过 inclusive fixed-map seek 跳过已完成 owner 的全部字段，只有完整 owner 扫描后才推进游标。完整目录预算、实际成员预算及 lookahead 都受限；部分扫描不能声称 exhausted，引用逐项核对 Manifest 身份，扫描不解码整个大值正文。

只有完成所有登记媒体扫描的 reclaimer 能构造私有回收凭据，绑定数据库、metadata generation、原生描述符及精确删除身份。writer 在同一次内容索引／Manifest Directory／Page 退役发布前复核；并发写使凭据失效。无凭据调用在内容索引规划之前拒绝；字段 REMOVE／owner 删除仍不能依据局部差集回收共享内容。GC 不改变原生描述符及字段语义 fence。

物理维护仅放行共享 immutable LargeData／Manifest 和保留内容索引池，原有当前 Page Directory、Catalog／allocator、全部旧 pin 和整池逆向路由证明仍保留。旧 pin 排空后允许退役池释放；通用图写入／通用协调仍受保护。native 活跃时的持久 State／journal GC 尚待接合；直接有界 GC 已接通，不能据此宣称所有维护入口完成。

新增8项定向用例通过：混合 ROW／native 和 Node／Edge、宽 owner／页预算拒绝与续扫、无凭据拒绝、保留5个当前值只退役1个孤立值、删除全部native大值后的整池释放、旧pin读、A/B两种恢复、扫描后的并发写拒绝。全部字段删除后的属性根正确为 missing，修正了夹具误用要求根存在接口的问题并保存失败记录。最终只运行27个相关存储模块：默认437通过；候选657通过、1个条件跳过。完整性检查通过；未运行全量单测或性能基准，仅验证Linux。

label／schema／derived-state协调、多组共享目录、完整成本／同执行器ROW对照及R7准入仍待完成。**无新增阶段验收，13/24（54.2%）；不提交、不推送。**
