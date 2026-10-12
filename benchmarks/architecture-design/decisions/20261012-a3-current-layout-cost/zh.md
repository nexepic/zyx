# A3 现有 owner 布局访问与编码成本起点

日期：2026-10-12。状态：调查与结构性对照，不是 S1.3 完整设计或 S3.2 布局准入。

## 访问链路

当前 PropertyCollectionRoot 将一个 Node/Edge 的 tokenized 字段集绑定到 owner。PropertyCollectionRootStore 先通过 Entity Property Directory 定位 owner，再解析 Page Location/Catalog/SlottedPage；大集合通过 owner reference 访问 LargeValue manifest/目录及所需片段。选择读取支持多个 token 共用目录校验，不能把它描述为每次解码全部值。范围路径也有 scan cursor 和 batch page cache，不能假定每个 owner 必然重新读取全部目录页。

| 操作 | 当前访问/工作单位 | A3 必须对照的额外成本 |
|---|---|---|
| 单实体单字段 | owner 定位、页身份认证、字段目录查找、选中值；大值增加 manifest/range 定位 | group/slot 定位与字段块寻址、presence/type 解释 |
| 完整实体 | 同一 owner 的实际字段目录及载荷，不枚举全库 token | 实际字段枚举目录及跨字段块读取，不能扫描整个组 |
| 跨实体少字段 | 多个 owner 的选列访问，复用共享页/目录状态 | 少数 token 的块扫描及存活/slot 映射，计入成员元数据 |
| 单字段修改 | 逻辑差量后仍构造完整 root 编码，原生页级 COW 与可能的 LargeValue 重编码 | 单页/局部块修改，避免改写整组及无关大值 |
| 删除/回收 | 删除 owner root 可达性，旧快照 pin 与既有 retirement | slot generation、presence、字段枚举、空块/组及 LargeValue 可达性一致退役 |

令 N 为 owner 数、F 为实际字段总数、K 为全库字段种类、P 为页大小。当前字段目录/载荷至少随实际 F 增长，而不是固定 N×K 稠密矩阵；完整 owner 编码工作随其实际字段及载荷增长。真实页面读取受 packing、目录高度、cache 与 LargeValue 布局影响，不能从编码长度推导物理页数或端到端复杂度。候选必须分别核验定位、枚举、presence/type、分配/索引元数据，尤其不能以新增 entity-property 独立树隐藏扫描收益。

## 不计时的编码证据

新增 PropertyLayoutCostBaselineTest 使用实际生产 codec，未创建假设格式。64 个整数属性的 owner root 编码为 1440 字节；修改其中一个整数后完整编码长度相同且内容不同，选列结果正确。包含 16 KiB 字符串和两个标量的 owner-neutral LargeValue payload 为 16083 字节，目录有 3 项，单字段选列及缺失字段返回正确。

这些是编码载荷字节，不包含 SlottedPage、manifest、目录、索引、allocator、发布元数据或日志，因此不是磁盘占用、读取量或写放大测量；不能将 1440/16083 当成真实写入成本。GoogleTest 的运行时长不用于性能结论。

严格构建和 2 项定向测试通过；原始日志/JSON、测试配置中的 RecordProperty 及源码 hash 位于 results/20261012-a3-current-layout-cost。新测试由仓库 GLOB 发现并纳入 selected module。没有运行性能 benchmark、全量单元测试、提交或推送，未改正式文档或英文记录。

## 下一步与未满足要求

尚需确定 bounded entity range/slot、分组页及局部载荷的具体编码候选，提供同时支持点读和真实字段枚举的索引结构，以及 generation/存活、组分裂、槽位复用、LargeValue 生命周期与失败恢复协议。必须以真实页面/字节计数验证混合形状、单次字段、类型变化和删除稀疏后的元数据边界；不能仅靠本轮 codec 样例验收。最终性能准入仍统一留到实现完成后。

进度：S0 25%，S1 14.3%（仅 S1.2），S2–S6 0%，总体 2/34（5.9%）。
