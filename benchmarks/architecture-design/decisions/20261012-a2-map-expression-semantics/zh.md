# A2 map 合并的表达式语义

日期：2026-10-12。两项新端到端测试在旧源码失败：SET n += CASE ... END 被 parser 的递归 map 搜索截断成第一个分支；SET n += {rank: 2, previous: n.rank} 被拆成逐字段赋值，previous 错读已修改 rank。先保存失败证据，再修复生产代码。

parser 现在保留完整 map 表达式，先求值再应用各字段，移除递归寻找第一个 mapLiteral 并拆成属性赋值的特殊路径。直接 AST 测试相应验证 map literal 仍是 SIT_MAP_MERGE 且带完整 expression，该变更由实际求值语义测试支撑。既有单字段 SET 及完整 map 替换不受影响；未修改 grammar。

模块回归进一步暴露 MERGE ON MATCH 原先依赖上述拆分。MergeSetAction 新增默认 PROPERTY 的动作类型，builder 显式映射，Node/Edge clone 保留类型，applyMergeActions 支持完整 map merge/replace，并仍拒绝未支持的标签动作。默认字段保持既有三参数构造语义。逐字段循环保留取消检查、NULL 删除、无变化比较与结果缓存同步。

最终严格构建成功，增加一个直接 AST 测试模块后显式选定 23 个测试源文件共 697 项、33 套件全部通过；定向 8 项在 parser 修复后通过，最终模块结果覆盖随后 MERGE 修复。没有执行全库测试。证据包括初始失败日志、最终构建/测试日志和 JSON、源码 SHA-256 及选定模块配置。当前仅 macOS 验证。

仍未完成属性容量预算、原生局部写入与 A3 布局生命周期。未运行 benchmark，未提交或推送，未更新正式数据库文档或英文记录。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
