### 2026-10-20 R3.2：带标签 Node 的字段／实体／LabelSet 原子删除

证据：`benchmarks/architecture-design/decisions/20261020-r32-labeled-owner-deletion/`。受控Node删除现在认证完整inline及overflow标签，逐个删除其独占LabelSet槽；同一源LabelSet页的多次删除合并成一份私有图像。其他Node的稳定slot locator与标签内容保持不变；非空标签页采用同PageId的私有物理COW，空页移除当前Page Directory路由。LabelSet、固定Node／Entity Map、原生字段目录／组头、Catalog／allocator及退役队列仍在同一次A/B发布。

源页及free-byte提示经类型化认证；整个Entity池仅在所有当前帧都被替换／移除时退役。旧pin可继续读取被删Node及原标签，排空后受限整池释放生效。标签页及编码／复制缓冲沿用调用方账户；额度拒绝和私有准备后取消不写源页／根，退款后可独立重试。仍要求无schema／derived State协调需求，Node删除的拓扑边界仍保守保留，不能据此宣称通用图写入已完成。

原inline标签拒绝夹具已在实现接合后改为验证成功删除、保留另一owner及旧pin；保存了原失败记录。新增6项标签删除用例覆盖未转换拒绝、共享页邻居槽、合并空页移除、全部owner删除后的旧pin／池释放、额度／取消及A/B两种恢复。最终只运行31个相关存储模块（加入既有LabelSet存储／公开合同模块）：默认460通过；候选691通过、1个条件跳过。未执行全量单测或性能基准，仅验证Linux。

schema／统计／索引／State及通用拓扑事务接合、完整成本／同执行器ROW对照和R7仍待完成。**验收进度13/24（54.2%）；不提交、不推送。**


### 2026-10-20 R3.2／R4.3：原生持久日志、受保护推进与有界窗口回收

证据：`benchmarks/architecture-design/decisions/20261020-r32-native-journal-reclamation/`。native 活跃时，专用 State COW 现可保存并恢复 GC 游标。保留身份 `.v2` 与旧 ROW `.v1` 等宽，payload 仍为原 version-1／固定容量，不新增 LargeValue 或字段根。通用 State writer 在两种构建均拒绝创建受保护身份；旧 ROW 进度在 native 活跃时不作为证据，并通过一次重置迁移。

普通 journal 只可开始／重置 manifest 枚举。owner 扫描结果带回收器私有签发的数据库／metadata generation／描述符／完整输出绑定凭据；保存时再次比对，不能修改候选、reachable、游标或 phase。非枚举阶段先匹配当前持久化的受保护 journal，最终回收也要求已持久化的完整 RECLAIM 证据；手写phase、篡改推进结果及旧 ROW 伪未来代数均不能授权删除。图写入推进generation使旧扫描失效；规范inline journal不再无谓阻止受控owner删除，未知State／schema协调保护保留。

State更新保持逻辑PageId／slot定位，COW追加到认证的当前State池；检查全部31位置（含bitmap-free）及所有活跃快照的精确物理路由，再复用无pin旧帧。钉满时换池，完整旧池退役并在pin排空后释放；不开放通用State写入或通用池协调。描述符／字段语义fence保持不变。候选窗口外的有效引用改为认证后保留，不能误报引用缺失；两个内建媒体的增量扫描可跨窗口完整结束，额外媒体的持久证明仍明确拒绝。

journal／推进凭据的ID向量改为实际计费的OwnedStdVector，跨scope返回仍持有额度，析构退款。测试覆盖70次同池复用、33个被钉住版本后的换池／释放、A/B恢复、关闭全部原句柄后的owner游标续跑、旧快照、拒绝／退款、保留5个活跃值且只退役1个孤立值的6窗口流程，以及伪造／篡改／旧日志迁移。新增13项定向测试通过；最终只运行34个相关模块（含State store与maintenance service）：默认497通过；候选738通过、1个条件跳过。未执行全量单测或性能基准，仅验证Linux。

这不是R3／R4阶段整体准入。原生池实际填充／完整分配成本、schema／统计／索引及通用拓扑事务、生产接入、同执行器ROW对照及R7成熟稳态仍待完成。**总验收进度13/24（54.2%）；不提交、不推送。**
