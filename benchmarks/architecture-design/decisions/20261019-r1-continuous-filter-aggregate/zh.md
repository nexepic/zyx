# R1.2 批过滤与聚合贯通验收

本轮最后一项基础能力交付；仅完成原定 R0.1/R0.2/R0.3/R1.1/R1.2。性能准入仍归 R1.3/R7，没有新增正式性能样本，也不宣称超越 Kuzu。

## 实现与替换边界

原生单元生产者直接填充 DirectPropertyColumns，复用绑定谓词、选择向量和 BatchEnvelope。字符串用借用字节参与比较和完整 tuple/DISTINCT key 编码；只在首次建立组及确有拥有需求时复制载荷，不为每个输入创建 LogicalPlanReadRow/property_cache。标量和混合类型保持类型身份，缺失/存储 NULL 的区别保留在批内，分组/计数遵守现有 Cypher 映射。

已有 AggregateAccumulator、组状态、DISTINCT 集合及拥有数据的输出继续使用。组键副本及状态增长通过 splitOff/absorb 转移预算，不退款后重新准入。源解码工作区与最终批所有者分别预约；新临时 owner/token/目录数组也在分配前准入。物理输入 ordinal 决定累加顺序，不能改成排序后的定位顺序。

直接批接入支持原生 NodeScan 及其单个安全残余 FILTER：属性在左、字面量在右的比较及 AND。FILTER 向子扫描转交批消费者；索引候选仍经过原来的行过滤器，不绕过索引或跳过残余条件。未知 provider、参数残余/复杂/易变/关联表达式、完整实体分组及 COUNT DISTINCT entity 保留有界适配器，后续按 R1.3 审计。无取值兄弟的全局 COUNT(property) 及不需要实际值的字段计数继续优先非 NULL 探测，避免无关大值物化。

只有交付前私有批构造的真实额度拒绝可以拆批；消费者失败不重放，取消回调即使抛出 MaterializedByteLimitExceeded 也直接传播。source owner/incarnation、快照/schema/token 支持身份以及 envelope 失效检查保持有效。没有磁盘格式、COW 确认、隔离、资源上限或 scratch 默认值变化。

## 验证与失败处理

- 首轮 10 个必要模块共 257 项，254 通过、3 失败。保留 first-run.json，不改写为成功。
- 修复真实接入缺口：安全残余 FILTER 的消费者转交，以及真实 BatchEnvelope 发布。新增封印、借用失效、取消同型异常和索引回退保护。
- 旧宽字段测试要求 16/32 KiB 必须失败，但重新编译冻结 c287 Runtime、使用匹配的当前 adapter/test ABI 时也不失败；旧单体二进制仍通过，不能作为当前冻结源码证明。保留对照。更正为检查空结果语义，并新增真实 1 KiB 额度拒绝；原有畸形列错误和重试断言保留。
- 最终显式选择 75 项全部通过（构建 11 个相关模块），覆盖 24 组残余比较/类型/NULL 对照、多键分组、DISTINCT、物理浮点顺序、私有批拆分、异常后重试、拥有输出、既有排序/连接/OPTIONAL 预算和重开退款。首轮及定向复核合计 266 个不同用例，不是一次 266/266 运行，未跑全量单测。
- 真实存储创建 600 个节点，跨至少两个批次，按 country/active 联合分组，对过滤后的 COUNT、COUNT DISTINCT、SUM 逐值比对独立 C++ oracle；新路径计数与输入总量一致，无旧输入行物化计数。建立属性索引后再验证残余过滤和每组 43 条匹配；后端关闭后返回字符串仍有效。

`proof.json`、source-manifest.json、相对已验收 R1.1 的 source-changes.patch 保存实际来源；新增测试文件由本提交及 manifest 定位。`run-necessary-modules.py` 复用已有 Ninja 编译/链接配置，只构建列明模块并执行明确 filter，参数为构建目录和结果目录。使用项目云环境工具链或本机已配置的等价工具链，不将测试耗时当作 benchmark。

R1.2 按 R0 事前登记的基础能力合同验收。R1.3、R2–R7 未计分；端到端性能、长期空间及与 Kuzu 的最终比较仍待对应阶段。
