# A3 token 读取认证回归

日期：2026-10-12。

新增真实存储用例，将合法页容器中的错误 inline owner、错误 LargeValue 引用 owner、错误 media type、非法零 token 分别发布。token-only 入口必须拒绝全部四种输入；前三种错误为 INVALID_STATE，非法目录为 INVALID_HEADER。验证输出与临时缓冲准入均未发生，读取失败不改变 generation。此测试只证明局部输入认证，不能证明全图不存在遗漏目录成员。

将现有 PropertyCollectionRootStoreValidationTest 纳入限定模块构建，发现其历史 materialized_bytes 断言触发 Werror 的有符号/无符号比较；预期小值显式转换为 int64_t，保持断言内容。五个限定模块共 57 项通过。构建、测试 JSON、源码 hash 保存于同名 results 目录。

后续继续 A3 生命周期与实际成本验收、统一预算及读取缓存。进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未更新正式数据库文档。
