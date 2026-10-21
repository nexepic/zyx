# R1.3／R5 保护的相关 OPTIONAL 输入合同

登记medium未计时正确性19/20通过；UNWIND重复／缺失gather与OPTIONAL(u:User{id:probe})组合报变量未解析。EXPLAIN显示Filter(u.id=probe)被放入独立于外部UNWIND的预物化pattern。保留完整规模失败，不删除或改称不支持该已支持语义。

复用现有计费的相关CALL／import／SingleRow适配器，为本lowering增加内部optional结果标志。仅当OPTIONAL表达式引用了输入中已有、而非pattern声明的变量时选择相关路径；不相关OPTIONAL保留原hash物化和求值次数。显式import投影为每次pattern调用提供输入，空结果仅将pattern新变量置NULL。保持重复／输入顺序、NULL、取消、输出所有权／资源上限、clone及重排边界。不新增parser能力／格式／并行执行器。用原始／优化计划、重开前后、缺失／重复／NULL／空外部输入及普通CALL／OPTIONAL必要模块验证，不跑延迟基准；15/24（62.5%）。

### 2026-10-21 R4.3／R1.3 完整规模准备与相关 OPTIONAL 修复

普通公共批导入实际走 read-your-writes overlay，不能将流式路径的计数归因给它。现普通全局邻接及 EDGE 成员索引均使用私有增量图像，并在原发布批次内认证与安装。完整规模再现的两个错误已修复：共享放置器的新池只登记一次；调用方已预留的 Entity SegmentId／PageId 在任何私有索引分配前进入批次身份空间，防止索引池与实体池重号。最终目的帧及 Catalog 身份另在发布前校验。十三个必要模块默认171/171、实验171/171通过，既有一项禁用保留；紧凑属性兼容279/279通过。

同一1000行批次、AS6GiB／文件2GiB上限，两套完整准备均成功：medium 225000节点／860000边，文件313655296字节；holdout 163000节点／772000边，文件264372224字节。没有测量导入延迟。此前9000边的身份冲突文件明确无效，不能用于正确性或性能输入。8MiB通用完整性扫描明确资源拒绝，不能记作通过；另用同额度只读流式核验实际页路由、主记录、双向邻接与类型成员，结果单列。

medium完整类型结果首轮19/20通过，剩余UNWIND＋OPTIONAL引用外部标量报未解析变量。复用现有计费CALL/import/SingleRow适配器修复相关作用域，保留重复、缺失、NULL、旧有不相关OPTIONAL与普通CALL。六模块默认186/186、实验186/186通过。新二进制仅重跑medium失败项并通过；holdout二十项一次全部通过，保留不同二进制来源而不合并成同次测试。包含多键／tuple DISTINCT、空聚合、多跳、完整实体、Top-K及重复／缺失gather。

证据：`20261021-r43-ordinary-index-images/source-identity-complete-working-delta.*`与`20261021-r13-correlated-optional-inputs/`。上述均为未计时正确性／工作量验证，不替代R3／R5生产选择或R7准入。已验收仍15/24（62.5%），无全量单测，无新增性能窗口，无提交／推送。
