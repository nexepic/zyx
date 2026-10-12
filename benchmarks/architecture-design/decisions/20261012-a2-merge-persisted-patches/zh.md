# A2 MERGE 持久实体补丁接入

日期：2026-10-12。MergeExecutionState 新增 query-local 属性补丁与 entry 工作上限。appendModifiedMergeEntities 在 snapshot 支持 shaped admission 时按原始 direct properties 与最终暂存属性生成 set/remove；变更恢复原值则省略补丁。否则保留完整替换兼容路径。标签与创建请求保留在 companion batch，同一次执行发布。

发布前仍验证 RETURN/ORDER BY 表达式；存在补丁时使用组合入口，源读取累计字节检查及取消检查保持与直接 SET/REMOVE 相同。所有 patch 候选由当前 attempt 生成，冲突后重新执行匹配、创建选择及修改，不能重放旧 patch。创建实体最终属性仍直接写入 create 请求，尚未改为创建 patch normalizer。

新增四项端到端测试：持久 Node ON MATCH 累加及标签修改一次发布、注入冲突后不重复累加；修改恢复原值不调用后端发布；源 admission 失败时属性/标签不发布；持久 Edge 完整 map 替换删除旧字段、返回最终值。测试以 shaped source 计数证明新入口被执行。冲突注入不是多线程隔离测试，资源检查也不是完整容量 owner。

严格构建成功，前三项定向通过，再新增关系测试重建后，显式选定模块共 714 项、33 套件全部通过。证据包括源码 SHA-256、选定模块配置、最终构建/模块日志与 JSON。仅 macOS 验证。未执行 benchmark 或全量单测，未提交/推送，未更新正式文档或英文记录。

A2 尚需 CREATE/FOREACH 持久修改接入、创建 normalizer、实际容量预算和原生按键消费；A3 局部属性布局及恢复回收仍需实施，不宣称旧布局已实现局部写入。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
