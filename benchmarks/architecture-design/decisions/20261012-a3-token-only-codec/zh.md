# A3 属性 token 枚举 codec

日期：2026-10-12。

新增 EntityPropertyCollectionRootCodec::decodeTokens，复用 owner、offset 边界、连续覆盖和正数递增 token 校验；校验完成后、数组分配前调用容量准入。只返回 token，不构造 PropertyValue，不修改现有字节格式。它依赖上层认证容器页，不替代属性值完整性扫描，也不减少当前 inline 根输入的读取字节。

新增定向测试覆盖 16 KB 字符串、NULL、稀疏 token、精确 token 容量准入、预算拒绝、零 token 损坏在分配前拒绝和空集合。限定四个相关模块共 23 项测试通过，构建启用 Werror。日志、JSON、源码 hash 位于同名 results 目录。

当前接口尚未接入 PropertyCollectionRootStore 和 PageBackedGraphReadAdapter，A3 原型仍会读取完整属性值。LargeValue 路径还需复用已认证目录及 owner 绑定，不能将本轮认定为生产 token-only 读取或布局完成。后续继续贯通存储读取路径及生命周期验证。

进度：S0 1/4（25%）；S1 1/7（14.3%）；S2–S6 0%；总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交、推送或修改正式数据库文档。
