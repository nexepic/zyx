# A1 快照绑定的保守 catalog 视图身份

日期：2026-10-14。状态：身份接口与消费者边界实现；最终模块验证结果见 results/20261014-a1-schema-observation/verification.json。S1.1 尚未整体验收，物理计划缓存未启用。

## 选择与真实存储协议

查询局部绑定增加 `LogicalGraphReadSnapshot::bindingSchemaIdentity()`，不支持的 reader 默认返回 nullopt。拥有数据的 `LogicalGraphSchemaIdentity` 含数据库/格式 domain、6 个 opaque catalog 视图 token、overlay view token 与 logical revision。它不含可借用指针，不修改文件 codec 或 Driver ABI。数据值与完整图可见性仍由原 snapshot/fence 负责；本接口单独不能成为完整计划缓存键。

第一次实现错误地使用 Superblock.catalog_root，新增属性名测试指出它并非原生 System Catalog 的树。实际 SystemCatalog 通过专用 RSS_SYSTEM_CATALOG_INDEX_ID 的 BTreeIndex 读名称、token 和定义。进一步验证表明，IndexRootLocation 的 root_page_id/root_generation 是逻辑页身份与生命周期：叶页 COW 后二者可以不变，不能当作 catalog 内容版本。测试中的新字段及约束定义断言因此继续失败，未降低这些断言。

最终身份采用 pinned Superblock 的 index_root_directory_root 与 page_directory_root 各三个位置/代次 token。前者解析 System Catalog 的逻辑根，后者解析整个逻辑 B+Tree 的 COW 页版本，覆盖根与子页修改。query 只比较 opaque 值，不解引用它们；metadata pin 提供其不可变来源。不扫描整个 catalog，不将 root PageId 的 lifetime generation 冒充独立 schema epoch。

这个视图刻意保守：其他索引或数据页修改也可能令目录 COW，导致绑定失效。它保证身份校验正确，未实现独立持久 schema epoch，也不承诺数据修改时复用绑定或缓存收益。最初“只改变值应保持身份”的测试期待不适用于这个真实页目录协议，已明确改为验证数据库身份、有效性及旧 pin；新增名字/定义必须使视图变化的断言保留。只有后续独立 epoch/能力/语义及完整物理描述符合同关闭后，才讨论跨查询缓存。

## 覆盖层、失效及错误

每个 LogicalGraphTransactionOverlay 获得不复用的进程内 view token，单调计数溢出明确失败；不同事务即使 base 相同、revision 相同也不同。普通与 vector overlay snapshot 均附加本 overlay 的 token/revision。成功语句变更 revision，失败还原保持原身份。事务完成后查询身份与其他 read 操作一样拒绝。新 view token 不改公开图 mutation fence 或冲突规则。

来源 reader 不支持时明确返回 nullopt；返回无效身份时抛错，不吞成可选优化退回。若来源本身是可变 overlay，保守拒绝把它作为本事务的不可变 base。该接口不保留页或 reader 指针，也不解除生命周期、输入参数类型、访问路径能力、语言/内核语义和 data snapshot 的其他校验责任。

## 定向验证及剩余工作

新增真实 backend 用例覆盖不同数据库文件、source pin、数据改值、新属性名称、schema 定义发布、close/reopen，以及两个并行存活事务相同 revision 的独立视图。普通/vector overlay 一致，成功更新失效、失败语句恢复、rollback 后拒绝；旧 fixture 不支持时保持 nullopt。覆盖层现有模块一起运行，具体结果见验证 JSON 与原始日志。未运行全量测试或 benchmark。

下一步仍为完整 immutable binding/physical descriptor、batch envelope 的 owning 动态值、显式兼容/移除清单与分组表示合同。此项是 S1.1 内的实际接口基础，不能单独增加验收分母或把未完成 S2 接入算成完成。当前验收 4/34（11.8%），百分比未增长，本轮代码及双语记录暂留工作树。
