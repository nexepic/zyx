# A2 完整 map 替换语义修复

日期：2026-10-12。新增测试发现 SET n = map 虽被现有 grammar 接受，却在 CypherASTBuilder 中误入 label 分支，执行时抛出 invalid SET label assignment。未修改测试绕过失败，而是修复源代码。

AST 与 logical action 增加有前缀的 SIT_MAP_REPLACE/LSET_MAP_REPLACE，保留既有枚举值顺序，LogicalPlanBuilder 显式映射。parser 在 property 与 PLUS 分支之后、标签之前识别 variable EQ expression，保留完整表达式而不拆成字段 merge。未修改 grammar 或生成文件，因此不需要 regenerate。

共享完整替换 helper 先验证 map 类型及非空字段名、过滤 NULL、构建新属性集合，成功后再交换暂存状态；连续 SET/REMOVE、CREATE/SET 与 FOREACH、MERGE 后续 SET 均显式处理该动作。SET n += map 继续合并；SET n = map 删除未指定字段；空 map 清空属性但保留标签；非 map 输入失败不能发布前面暂存修改。该 helper 的容器容量仍未完成 A6 实际分配计费，不宣称资源闭环或局部物理写入。

新增五项测试覆盖完整替换在补丁/旧替换两路径上的一致性、类型变化与 NULL、空 map、补丁源 admission 失败早于 backend 调用、CREATE/MERGE/FOREACH 替换及非 map 失败原子性。fixture 独立源 admission hook 在复制前触发，不将后端发布控制失败冒充源准入验证。

初始定向运行暴露两项真实失败，修复后八项补丁测试通过；新增两个跨写链与失败测试后再次构建，最终显式选定模块共 689 项、32 套件全部通过。证据目录保存最终构建/模块日志与 JSON、修复后的定向日志、源码 SHA-256 和选定目标配置。当前仅 macOS 构建测试，不声称 Linux/Windows 已验证。全部新增源码注释和错误为英文。

未执行 benchmark、全量单测、提交或推送，未更新正式数据库文档或英文记录。后续仍需原生按键消费、创建 ordinal 与 mixed batch 合同、容量计费以及 A3 布局生命周期验证。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
