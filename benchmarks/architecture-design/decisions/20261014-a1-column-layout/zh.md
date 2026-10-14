# A1 不可变输入列布局与逐批聚合校验

日期：2026-10-14。状态：S2 的列绑定边界已接入一个生产聚合路径；全物理程序/layout/data-snapshot envelope、属性 token 消费接口及所有算子尚未完成。总验收保持 5/34（14.7%），不新增完整验收项。

BoundColumnLayout 是不可复制/移动的 query-local owner。它在共享预算 PMR vector/string 中拥有有序字段名，并分配非复用布局身份；BoundColumnId 保存 uint32 索引和 uint64 布局身份。外来、同地址重建、零身份及越界描述符被拒绝。原 producer 字符串销毁或改变不影响拥有的名称。名称布局不代表固定值类型，同一字段不同批次的数值/动态类型仍须经既有 typed consumer 的逐批推断。

构造检查 uint32 列数、非空及重复字段名，重复校验使用计费的 PMR set，避免两两比较。校验节点只在构造期间驻留，完成后释放；有序名称及字符串容量在 owner 析构前持续计费。分配请求与扩容重叠经 BudgetMemoryResource 在 upstream 分配前准入，失败退款。每个输入/构建项调用查询 guard，bad_alloc 与取消不转为“不支持优化”。空布局合法。普通 string_view 借用依然必须服从 layout 生命周期，不把裸 span 变成认证 view。

发现并修复的生产边界：LogicalPlanReadRuntime 的 Node 列聚合只在首批按名称解析列号，后续批次直接使用数字索引。现在每个回调在累计任何行之前检查列数、每列行数与 selection 一致、所有 original ordinal 在界限内；随后验证首批绑定的布局名称及顺序。首次布局 metadata 准入压力可在效果发生前选择明确名称适配器；适配器每批检查已绑定索引仍对应聚合 AST 中的预期字段名。只捕获 metadata 的 MaterializedByteLimitExceeded，其他错误传播，不重放已累计行。布局在聚合输入结束、下游输出前释放，避免持有名称缓冲挤占输出预算。未迁移 COUNT(*) row-only 或 grouped row/evaluator 路径，不宣称全算子接入。

本轮没有观察到实际合法 producer 改序导致的错误查询；这是补齐原首批绑定代码缺少的消费边界。人工改序/改名/增减列的组件测试证明校验在消费者效果之前失败；现有多批次、逐批类型变化、损坏 source、紧预算可选转换释放与动态重试回归仍通过。新测试文件已列入 cmake/SourceSets.cmake 的 native allowlist。

验证：新增 7 项布局测试，包括名称拥有化、顺序/宽度变化、外来/旧身份、非法名称、分配前预算拒绝、每个观察到的 upstream 分配失败及 guard 失败的退款/重试。组件/Node 聚合模块 75 项与相关运行时模块 222 项全部通过，重叠 26 项，共 271 项独立测试。Linux/C++20，仅必要模块；未运行全量、性能或 Kuzu 比较。新增测试的 range-copy 编译警告已消除；既有 property_tokens 初始化警告仍存在。

证据在同名 results 目录，保存构建/模块日志、链接命令、源 SHA-256 与 verification.json。基线仍为已推送 884ffcf2；本轮及此前 batch incarnation 改动留在工作区，验收比例尚未增加，未自动提交/推送。下一步继续补齐动态值 owner 与物理程序绑定，最终性能窗口后再验收 O1/O2/O4。
