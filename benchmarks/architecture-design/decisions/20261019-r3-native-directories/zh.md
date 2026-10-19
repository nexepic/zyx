# R3.2 共享映射的原生基础能力

父提交 `9f7733ed`。R3.2 保持进行中，总验收仍为 13/24。本次在五个 payload/门禁步骤之后推进目录前置能力，不验收原生混合属性完整生命周期或 R3.3；先登记后变更。

现有 B+Tree 叶值只有 U64 或 EMPTY。本次保留格式和通用 entry 大小，以 EMPTY 叶的“有序键前缀 + 固定 canonical 值后缀”实现定宽映射；R3.1 定案明确有序键用大端、记录体用小端。逻辑宽度仍为 owner 12+4、列定位 12+16、组定位 8+16，实际 packed framing、restart、内部分隔键和原生分配另计；不宣称实际空间已经改善。

`FixedBTreeMap<KeyBytes,ValueBytes>` 提供定宽编码/解码和一次有界前缀读取加前看。两个不同原生键共享同一逻辑前缀即视为损坏。完整页认证、快照路由与跨叶遍历继续由既有 BTreeIndex 执行。helper 检查宽度及 EMPTY 叶值；类型化调用者仍须认证 locator 指向的 owner/type/generation，并执行全局双向 integrity。它没有实现转换 writer、完整成本估计、extension 变长值 codec 或 hybrid 图路由。返回数组无堆分配；cursor/返回键的动态内存仍须既有 native memory scope 管理，回调仅准入 cursor boundary key。

修改映射须在同次发布中 erase 精确旧完整记录、insert 新完整记录。后续拥有该映射的规划器须把每个逻辑前缀归并成事务内一个最终替换，不能从同一旧快照盲目多次 stage，否则会形成重复前缀。不自动修复，也不采用最后值覆盖掩盖错误。

NativeGraphWriteTransaction 现在在复合 index 暂存、普通规划及 fresh-pool rebuild 中保留显式 EMPTY 叶布局，默认仍为 U64。未知布局、EMPTY 非零值、同一 index 混合布局或异域 page store 均拒绝；整树退役继续复用既有原生协议。未分配 hybrid MetadataPageKind，未改变 Superblock 31/profile 5。测试只在实验命名空间持久化既有格式的通用 index 和合成字节，没有持久化 hybrid 描述符/列格式或安装属性路由。

## 证据

九个必要模块、11 个 suite 共 178 项通过（`final-178.json`）。首批六个新增检查及既有模块共 174 项通过（`first-174.json`），追加四个生命周期检查后为 178 项，不是 352 个不同用例。新增十项覆盖精确大端前缀/小端定位值、原生复合替换、稳定 root PageId 的 COW、旧 pin/重开、缺失前缀、重复逻辑归属、布局变更拒绝、512-entry 多叶点读/更新、admission 抛出/重试、保留全部旧代际的 36 次更新耗尽原页池后切 fresh pool、整映射退役、三种 bootstrap 中断及禁止把 U64 树重解释为 EMPTY。既有 scalar/vector/label index、共享页池 fallback、durability、transaction overlay 和 initial tree 模块保护默认行为。未跑全量单测或正式性能测试，实际仅验证 Linux x86_64。

使用已配置 native Ninja/toolchain 环境运行 `run-necessary-modules.py BUILD_DIR OUTPUT_DIR`。`test-scope.json` 记录模块/suite 与精确链接；runner 只枚举全套目标的命令，实际仅编译链接选定对象。源码 hash 见 `source-manifest.json`。

下一步仍是按 R3.1 完成原生 descriptor/root/fence/page-kind/version 与读写/完整性/搬迁/退役原子更新、共享列/extension 页、计费双向有界转换/降级、原生真实成本及同执行器 ROW 对照。不能把通用 map 的生命周期替代整个 R3.2 属性生命周期，也不能放宽 R7 门禁。
