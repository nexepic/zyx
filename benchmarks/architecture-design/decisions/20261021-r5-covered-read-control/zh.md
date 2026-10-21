### 2026-10-21 R5.1／R5.2：同执行器的显式只读覆盖邻接对照

证据：`benchmarks/architecture-design/decisions/20261021-r5-covered-read-control/`。增加显式openCoveredAdjacencyCandidateReadSnapshot：只允许物理只读backend，使用调用方账户分配快照对象，保留其真实metadata pin；setup显式执行有独立Edge／metadata页上限和取消的双向全局integrity。不是普通点读／查询规划中的全库扫描，也不接受外部报告授予权限。默认读入口不变，不创建writer hook／生产namespace／格式开关。

候选只接单源和多源reference批路径，其他接口保留原实现；批接口完整验证页后才调用消费者，保持有序unique输入、总Edge上限和提前停止。每页最多64条但不是总库上限；支持90边超级节点跨页、空树且无Edge的证明及Node存在性检查。逻辑continuation没有物理身份，必须在该pin中验证previous成员与精确next；首EdgeId=1的前驱验证使用initial scan，不放宽私有协议的非零continuation要求。输出容量通过既有buffer-admission交给消费者持有；私有工作空间及setup取消／额度拒绝退款。主Edge认证仍保留。

同一个LogicalPlanReadRuntime对照key-only／covered，使用已绑定源的WITH扩展，实际reference_pages必须大于0；验证OPTIONAL／聚合／重复邻居gather、hybrid属性和原生字段修改后的相同结果。全Edge扫描计划本来不经过邻接，不能仅因结果相同声称候选被调用。初版缺批接口及首continuation拒绝的失败记录保留。新增损坏缺反向项／合法CRC错误endpoint拒绝、预算、取消、continuation、批顺序／stop／总量、只读权限、超级节点和空图回归。

55相关模块：默认879通过；候选1205通过、1条件跳过；成熟多组空间已有独立证据。本轮无全库单测或性能基准。该入口仅为实验对照，尚无自动布局选择／生产采用，overlay和剩余生命周期／成本／R7继续推进。**总计13/24（54.2%），验收增长0，不提交、不推送。**
