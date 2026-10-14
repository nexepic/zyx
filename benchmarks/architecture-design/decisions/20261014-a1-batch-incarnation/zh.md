# A1 批次 owner 与 incarnation 校验

日期：2026-10-14。范围：S2.1 的批次行身份基础及一个真实聚合消费者；完整 envelope、layout/data-snapshot 身份、动态 arena 和其余算子迁移仍待完成。总验收保持 5/34（14.7%），本轮不新增验收项。

新私有组件 BatchIdentity 使用共享于不同列类型的非复用 uint64 owner 和 uint64 incarnation；BatchRowId 是 uint32 行号加 owner/incarnation，不保存缓冲或页指针。owner 计数耗尽明确失败。incarnation 不回绕；append 在变更前检查耗尽，noexcept clear/release 在耗尽时永久禁用描述符，之后 append 明确失败。未实际执行计数耗尽压力测试，不宣称覆盖该边界实测。

FixedWidthBatch 的 rowId/cell 校验 owner、incarnation 及已发布行界限，再返回拥有的固定类型 TypedCell，保留 missing/NULL/value 与行次序；重复值仍有不同物理位置。成功 append、可能已重定位早期列的失败 append、clear 和 release 均终止旧描述符。行限额在任何列变更前拒绝，因此该失败不终止有效描述符。矩形失败回滚保持旧数据与真实已分配容量计费；可重新取得当前 incarnation 的行描述符读取原值。clear 保留已计费容量，release 释放。重建同地址 owner 不接受前一 owner 的描述符。行数限额明确为正 uint32 范围。

PrimitiveNumericColumns 使用同一身份协议。仅在 guarded prepare 全部成功后发布行数与可取得的描述符；失败或取消不发布。release 使旧描述符失效并释放转换缓冲，之后重建批次也不能复用旧身份。空 prepare 也有明确 published 状态，须 release 后重用。update 的描述符重载在任何累加器变更之前认证 owner/incarnation/行界限；原 size_t 入口作为明确适配器获取同一描述符，再调用校验入口。

生产接入：LogicalPlanReadRuntime 的数值列聚合调用使用 numericColumns.rowId(ordinal)，消费带身份的描述符。COUNT/SUM/AVG/MIN/MAX/STDEV/STDEVP 既有 typed/dynamic 算法保持；可选转换缓冲在预算压力下释放后走已有逐字段动态重试，不重放已累计行。COUNT(*) 的 row-only 路径不需要数值列描述符。普通列 span/column 接口仍保留，其借用生命周期由调用者保证；本组件不把任意旧 C++ span 转为安全 view。程序/layout/快照身份、动态值、选择 bitmap 与全算子描述符统一仍是后续任务，不宣称完整 S2.1。

验证：新增 7 项 FixedWidthBatch 测试及 3 项 PrimitiveNumericColumns 测试，覆盖三态/有序重复行、成功与失败 append、clear/release 复用、外来/零/越界描述符、跨列类型 owner、同地址重建、prepare 取消与重试、空批次状态，以及拒绝发生在累计效果之前。组件+Node 聚合模块 68 项、相关运行时模块 222 项，重叠 26 项，共 264 项独立测试全部通过。核心和相关测试对象从当前源重编译。Linux/C++20；未运行全量测试或性能/Kuzu 比较，不能据此宣称吞吐改善或跨平台实测。既有 property_tokens 初始化警告仍存在。

证据在同名 results 目录：构建日志、模块链接命令、测试原始日志、源 SHA-256、verification.json。基线为已推送 884ffcf2；本轮尚未提交/推送。后续优先补齐物理 layout 与有预算准入的属性描述符消费接口，保留空输入的延迟错误语义。
