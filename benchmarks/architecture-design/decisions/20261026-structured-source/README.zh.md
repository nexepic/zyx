# 源码职责拆分验证

本次结构工作不计独立性能进度，不改变持久格式或 API。性能候选的验收另见 scan-supply-v1。

- 写规划器按实体、向量、邻接、提交、生命周期划分为真实编译单元；元数据按页面修改、回收、查找划分。方法体保持原内容，见 storage-splits.json。目录两项批读取依赖私有游标定义，保留在原编译单元，未暴露私有实现。
- 查询运行时、事务覆盖层及大型存储流程按责任阶段提取私有 .inc；保持同一编译单元、匿名命名空间和 RAII 生命周期，展开哈希见 inline-splits.json。
- ANTLR 生成器增加确定性规则族拆分，WASM 生成器提取独立文件系统模块。重新生成后也执行 3000 行约束；不只修改当前生成产物。
- IMDb 数据分文件后条目与顺序不变，校验记录见 scan-supply-v0/seed-split-verification.json。
- 12 个历史超长源码快照作无损归档，见 ../../archive/20261026-oversized-source-evidence/；旧实验重放应先按说明恢复，不能误认为旧脚本无需准备即可运行。

验证：扫描及在线格式核心库当前源码均已编译；相关模块 277 项通过，详见 scan-supply-v1/final-module-receipt.json，其中 ROW 元数据采用明确列出的当前源码覆盖验证。WASM 检查使用仓库现有二进制，真实 Driver ABI 打开、事务和查询以及两实例文件系统隔离通过，见 wasm-runtime-test.log。未重新编译 WASM 二进制，未执行 macOS/Windows 或完整浏览器构建，不声明这些平台已验收。代码尺寸检查由 scripts/check_source_size.py 执行。
