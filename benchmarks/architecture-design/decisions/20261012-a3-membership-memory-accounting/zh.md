# A3 原型目录内存合计

日期：2026-10-12。

原型 buildFromNativeSnapshot 改为绝对合计准入：已构建目录的成员与 token vector 实际容量、当前源 token 数组、LargeValue 目录临时缓冲同时计量。成员数组在 reserve 前准入，token 目录扩容前计入旧数组及新请求数组；reserve 后回报实际目录容量。源 token 显式释放后扣除容量，正常返回前最终回报等于 metadataCapacityBytes。合计加法及 token 容量乘法有溢出检查，取消检查覆盖准入前后。

真实 64 字段快照测试验证峰值大于最终目录容量，最终回报相等，以及仅够最终目录但不足构建峰值的预算被拒绝。原有取消用例现在允许校验前的成员目录准入，codec 层仍独立验证 token 校验阶段。五个限定模块共 58 项通过，证据在同名 results 目录。

尚未覆盖 NativeReadSession、activeOwnersSorted 临时容器、inline 页缓冲、allocator 自身开销及精确超额容量的扩容重叠；因此不是统一查询预算，也不是 RSS 上限。标准 vector 的实际超额容量只能在 reserve 后获知，严格分配前预算仍需预算 allocator。当前按 owner 精确 reserve 可能带来最多 64 次 token 目录复制，成本需纳入 A3 准入。真实页生命周期、恢复回收和成本表仍待完成。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未更新正式数据库文档。
