# A3 紧凑目录解码与 owner 表绑定

日期：2026-10-12。实验格式修订为 v2；未接入生产或现有组 descriptor。

上一轮编码 v1 未绑定完整 owner 表，现增加 32 字节 SHA-256，header 扩为 80 字节：原 offset 0–44 字段保留，version=2，offset 48–79 为整个 canonical slots.encode() 的 digest。counts 从 offset 80 开始，token 从 336 开始。CRC 仍在 offset 40，计算时置零。旧未绑定 v1 明确拒绝。

decodeSlotCounts 先检查格式/身份、完整长度、字段/字节界限、owner 表 digest 和 CRC，再验证所有槽位计数、空槽不得有字段、每个 owner 的 token 正数递增及计数完整覆盖。全部通过后构造 owning token/member 目录，不解码 PropertyValue。保留 fieldless active owner。

新增有效 CRC 的零 token、空槽计数、保留字段和旧版本损坏测试；合法但不同 owner/slot generation 表的混配被拒绝。64 owner 共享/独有字段均往返通过，字段上限拒绝。五个限定模块共 67 项通过，证据在同名 results 目录。

新目录大小为 592 字节，而非上一轮未绑定格式的 560；相比原 2352 字节仍减少 1760。固定开销现在为 80+256，原格式为 48+32×owner 数，因此 9 owner 时相等，少于 9 旧格式更小。后续格式选择必须采用更新阈值。原生组 descriptor 暂时仍使用旧 directory codec，不暗中切换格式。

解码临时 SHA 输入、CRC 副本、路由数组尚未统一预算，token 循环尚无细粒度取消。持久子块共享页、路由、可达性和回收仍待推进。进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
