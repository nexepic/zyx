# R3.2 原生接入前的可复现检查点

本轮固定交付为 R3.2、R3.3、R5.1、R5.2、R5.3。当前五项均未完成，不将实现子步骤计作阶段交付。主文档分母与准入条件保持不变。

`FixedBTreeMapMutation` 对现有原生共享树按逻辑前缀合并最终状态：一次 erase-old/insert-final，反复 SET/REMOVE 不形成重复 suffix。最终 no-op 不 stage；原生空事务仍拒绝 commit。PMR 计费保留的 map 节点；cursor boundary admission 可单独传递，原生 cursor/source 与 transaction staging 缓冲仍由各自所有者计费。必须独占该事务中此 index 的操作；它不能替代首次建树、全局完整性或最终 locator 的身份分配。staging 失败回滚整个图事务。Native rollback 改借用 ID 预约范围，不再为复制列表分配内存。原生旧 pin、重开、拒绝/独立重试和 inactive 状态受定向测试保护。

`ReservedGroupConversion` 仅作有界实验规划：实际账户预约固定 holder 与工作空间（上限低于 1 MiB），最多 64 个真实 owner、4096 个成员。验证原生 owner kind、正数且不超过 INT64_MAX、组身份、唯一 owner 和严格递增 token；按真实字段选列，不按潜在全局 token 展开。48 个 present 槽且非 NULL 类型一致的 int/double/bool 可选列；全 NULL 无类型推断，混型、稀疏及其他类型保留为借用的 canonical extension 输入。NULL 与 missing 区分，浮点原始位保留。反向 owner 投影只访问真实字段并保持 token 顺序；大值借用不复制，其源内存由调用者另行计费和保持存活。输入必须不可变并活过 plan，budget 也必须活过 plan。该规划不运行成本准入、不持久化 extension、不生成 LargeValue 引用、不安装图路由，也不代表原生正反转换完成。

最终四个受影响模块、六个 suite 共 53 项通过。`final-53.json` 是最终原始结果；`planning-final-51.json` 是添加账户/状态保护前的检查点。首轮 45 项中两项 fixture 预期与既有合同不符：no-op 事务应拒绝空 commit，inactive commit 抛 FormatException；保留 first JSON，修正测试而非放宽原生合同。新 group planner 的首编误用了 PropertyValue 私有 data，改用公开 getVariant，保留编译失败日志。没有运行全量单测或正式性能/Kuzu 窗口，仅验证 Linux x86_64。

下一步仍必须完成 R3.1 原生格式/root/fence/reader/writer/integrity/relocation/retirement 的完整原子接入。固定值后缀是 key 的一部分，不能用禁止改 key 的 InitialTreeValueBinder 回填 locator；应由同一次发布中的内部 metadata 协调器先分配数据 PageId，再规划目录，最终八个树根身份确定后写 descriptor。不得另行提交 ID 或沿用七根 fence。完成 R3.2 后才可将获准方案接入 R3.3，随后完成 R5 三项和统一 R7 准入；本检查点不缩减任何交付条件。

追加的原生描述符 codec 检查点：192 字节逻辑 payload 加 32 字节物理认证头，数据库/物理根代际/语义代际/LSN/八根独立身份及全页 CRC/保留位均验证；逐字节损坏和合法校验和下的错误身份拒绝。搬迁保留语义代际、更新物理代际。详情纳入 R3.1 决策文件。仅内存 byte buffer，没有文件 writer、Superblock/root 激活或完整生命周期。追加后五个必要模块、七个 suite 最终 58 项通过，原始结果 `final-58.json`、范围 `test-scope-58.json`；此前 53 项是更早检查点。本轮五项完成数仍为 0。
