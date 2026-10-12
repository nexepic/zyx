# A3 目录循环取消控制

日期：2026-10-12。

inline decodeTokens 与 LargeValue decodeDirectory 新增可选 CancellationCheckpoint，校验与复制逐项检查。既有调用默认无 checkpoint，字节格式不变。PropertyCollectionRootStore token 入口将 checkpoint 传入目录解析，并在输出 token 复制时检查；目录读取助手在入口检查。读取适配器与原型尚未传递此 checkpoint，下一步继续贯通，不能认定端到端细粒度取消已完成。底层 chunk I/O 的取消边界仍待验证。

新增 64 字段定向测试，在第 8 次和第 70 次检查抛出，分别覆盖校验与复制阶段，断言检查次数、准入次数及中断后的正常重读。四个限定模块共 26 项通过。构建及测试证据位于同名 results 目录。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
