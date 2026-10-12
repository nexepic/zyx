# A3 字段 presence 不可变更新

日期：2026-10-12。

新增候选目录 withFieldPresence：针对一个有效 owner route 增删 token，校验当前快照、block/type、全部 owner 槽位及字段上限，新身份要求同 database 且 generation 严格推进。生成独立目录，保持旧目录不变；仅复制 token 与成员目录，不解码值。NULL 属于存在字段，删除则移除 token。

新增目录与稀疏列配对测试：新增 NULL 字段后新目录与新列匹配，新目录与旧列不匹配；旧目录旧列仍匹配。删除另一个 owner 的字段后 presence 正确，字段预算、零 token 与不推进 generation 拒绝。五个限定模块共 60 项通过，Werror 构建、测试 JSON 及源码 hash 保存于同名 results 目录。

此接口为原型更新基础，不具备生产原子发布能力。目录与列必须在未来同一组 descriptor generation 发布；当前列 view 仍外部借用，不能据此信任持久引用。目录更新成本为全目录 token/成员复制，尚未接预算 allocator。后续需补齐组级更新、空列退役、持久页发布、恢复/回收及完整成本验证。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
