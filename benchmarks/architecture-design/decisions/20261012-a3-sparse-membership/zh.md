# A3 稀疏列与实际字段目录的一致性

日期：2026-10-12。

字段枚举候选新增 validateSparseColumn，稀疏列的行号明确为稳定物理 slot（固定候选范围 64），不同于旧 TypedPropertyBlock 的排序 owner row。先验证 snapshot/block/type、实际 owner 数和列 codec，再逐成员校验完整 route，按实际 token 集计算 expected presence。存在 NULL 仍贡献 presence；未映射 slot 的载荷、漏字段、错误 token、owner 集变化拒绝。零成员 token 的空列不能继续留在 group directory，后续发布须退役该列。

新增用例覆盖逆序 slot/owner 排列、NULL、未映射值、遗漏字段、错误 token 和插入新 owner 后的旧目录。严格构建与相关四个模块 16 项测试通过。证据位于 results/20261012-a3-sparse-membership，含日志、JSON 与源码 hash。

这里只校验单列与目录关系，不证明整个组没有遗漏列、值与存储记录一致或已具有持久页身份。完整组原子发布、列目录 codec、slot 复用、LargeValue 与回收协议仍待实现。实验容量不是最终组参数，不准入生产。

未运行 benchmark、全量测试，未提交或推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
