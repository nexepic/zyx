# A2 补丁生成查找表的实际分配准入

日期：2026-10-12。finalPropertyPatch 的原属性 lookup 改为 pmr unordered_map，使用现有 BudgetMemoryResource 与局部 MaterializedByteBudget，三类调用路径均传入 maximum_materialized_bytes。分配器在 upstream 分配前准入节点、桶和重哈希重叠的实际请求字节，分配失败回滚，容器销毁后再销毁 resource/budget，避免生命周期颠倒。string_view/PV 指针只借用当前 entity 的原始载荷，不拥有另一份数据。

局部预算只覆盖该 lookup，不覆盖同时存活的 mutation result、源、patch 字符串/PV 和其他暂存容器，也没有接入数据库/进程总预算；不得以它宣称整条查询容量已经受控。后续需统一查询 owner，避免多个独立限额叠加。分配器失败不会被当成可选性能回退吞掉，原子发布仍在补丁生成和结果验证之后。

严格构建成功，相关 mutation runtime 201 项与 BudgetMemoryResource 7 项共 208 项通过。沿用已有分配器测试验证分配失败、上限及归还行为；本轮未新增复制实现的测试，也未声称这些单测证明了全局预算。未重复执行不受影响的其他读取模块，更没有全量单元测试。

证据包含最终构建/模块日志与 JSON、源码 hash 和选定模块配置。仅 macOS 验证。未运行 benchmark、提交或推送，未更新正式文档或英文记录。原生局部属性写入与 A3 布局生命周期仍未完成。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
