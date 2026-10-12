# A3 退役候选引用差集

日期：2026-10-12。

核对现有 metadata 发布接口支持 retiredLargeValuePages、erasedLargeValueIds 和 retirement preparer；LargeValue 提供 manifestTree、planPageRetirement 与 snapshot 延迟复用。因此可复用原生协议，但必须先证明对象不可达，不能仅凭局部组更新退役共享子列。

新增 SparseGroupRetirementCandidates::collect，仅计算旧 root/列与新 root/列的引用差集，按 value ID 排序去重，拒绝零 ID 或同 ID 不同不可变引用。明确结果是候选，不是可达性 proof，没有调用物理 retirement。其他组仍可能引用候选，需要权威 route walker 或等价证据后才可执行。

真实发布测试扩展验证修改一个字段产生旧 root 与旧列两项候选，未改变组无候选，相同 ID 不同引用拒绝。删除布尔字段的最后一个成员后只新增 descriptor，整数列引用保留；旧 root 与旧布尔列成为候选，读取正确显示字段缺失。五个限定模块共 65 项通过，Werror 构建、日志、JSON 和源码 hash 在同名 results 目录。

后续继续权威组路由、完整可达性、同次发布退役与旧快照物理回收验证。不能把候选差集认定为真实回收或 A3 生命周期已完成。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
