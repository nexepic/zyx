# A3 完整稀疏列组覆盖校验

日期：2026-10-12。

单列 presence 校验不能发现整个列缺失。新增 validateSparseColumns，使用同步借用的 token/bytes views，要求正 token、有序唯一列、列数不超过实际字段数，逐列检查 codec/presence，再确保每个实体实际 token 都有列。读取 group 的 snapshot/block/type/owner 集，并显式验证所有 route，包括字段数为零的实体，避免没有列时跳过槽位代际校验。

新增测试先证明保留下来的单列仍能校验成功，再删除另一列，完整组入口必须拒绝；重复列/逆序列也拒绝。无字段实体在同槽位被另一 owner 复用、活跃 owner 数未变时仍拒绝。严格构建与相关四个模块共 18 项通过，证据位于 results/20261012-a3-complete-sparse-group，含日志/JSON 和源码 hash。

此入口没有持久 proof/cache，不持有或冻结 caller buffers，要求校验期间输入不被修改。完整元数据关系校验不是存储发布/崩溃恢复的证明，也不证明值与另一权威副本一致。下一步仍需组 descriptor codec、原子可达性、Mixed/LargeValue 路径及回收协议。未生产接入。

未运行 benchmark、全量测试，未提交或推送，未修改正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
