# A3 存储层 token 读取

日期：2026-10-12。

新增 PropertyCollectionRootStore::readEntityPropertyTokens。要求有效逻辑 owner、属于当前 metadata store 的快照及两项非空准入回调。定位目录后验证页身份、页 owner kind、槽位 generation 和根 owner。inline 使用上轮目录 codec，LargeValue 复用 manifest、range cache 与 token/offset 目录校验，验证引用 owner、种类、media type 和完整逻辑长度，不解码属性值。缺少属性根返回空 token 集合。

输出 token 容量与 LargeValue 临时目录缓冲分别准入；成功释放目录后报告临时容量为零。两者尚非统一查询预算，也不覆盖 inline 输入页或复制式页读取缓冲。异常时调用者仍负责撤销本次准入状态，不能将回调最后数值当成实际存活内存。LargeValue 读取按认证 chunk 进行，不能宣称物理读取字节恰好等于目录长度。

真实 native planner 测试覆盖 inline 的 NULL presence、字段修改后新旧快照、输出预算拒绝、空准入拒绝；另以 16 KB 字符串构造真实 LargeValue，覆盖目录缓冲准入、输出准入、拒绝后同快照重试。四个限定模块共 25 项通过，构建开启 Werror。证据在同名 results 目录。

PageBackedGraphReadAdapter 与 A3 buildFromNativeSnapshot 尚未消费此入口，原型目前仍完整读取属性值。尚需接入、取消控制、统一预算及完整生命周期验证；不计为 S1.3/S3 完成。

进度：S0 1/4（25%）；S1 1/7（14.3%）；S2–S6 0%；总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
