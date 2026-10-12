# A3 原生稀疏组读取载体

日期：2026-10-12。

新增 NativeSparseGroupReader 实验读取载体：检查 descriptor 引用种类、media type、逻辑字节上限及 birth generation 不超过读取快照，通过现有 LargeValueStore::read 验证 descriptor 与各子列的原生引用及内容 hash，再 decode 字段目录/引用集合并 freeze 完整拥有的稀疏组。子列 codec 与目录 presence 在 freeze 中交叉验证。root media type 为 application/vnd.zyx.lab.sparse-property-group。调用者必须从当前快照权威路由获得 descriptor 引用和其 birth version，此载体自身没有权威图路由。

真实 LargeValue 写入测试验证完整实体恢复、预算拒绝、合法 descriptor 引用的列 presence 错误被拒绝、旧组仍可读取。释放快照和 LargeValueStore 后重开 metadata/native stores，使用保存的根引用恢复相同实体。此处重开 metadata 与原生 store，未关闭宿主文件句柄，不能替代完整 backend close/reopen 或 crash 测试。

五个限定模块共 63 项通过。构建启用 Werror，日志、JSON 和源码 hash 在同名 results 目录。测试准备使用独立原生 writes，不是新生产原子发布路径。

尚未实现原子组 writer、权威 owner/group 路由、引用 retirement 和故障回收。读取暂时加载所有列，freeze 复制载荷一次；预算仅复用单次 native read 准入，不包含全部累计缓冲与 owning group，不能称统一预算。codec 内部和单次 I/O 取消粒度仍有边界。下一步继续原子写载体与完整生命周期。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
