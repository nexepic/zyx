# S1.1 A1 完整设计交付验收

日期：2026-10-14。结论：S1.1 设计交付通过；S2 生产迁移及其语义/资源/性能验收不通过、仍待完成。完整合同为同目录 zh.md 与 design-profile.json，不以组件存在替代完整执行器。

依据 optimization.mdx 第 3 节设计交付清单及 A1 的批次、图中间结果和执行状态合同。S1 允许具体决策、证据或明确后续实验；正确性/核心接口决定须在相应生产接入前关闭。本记录以 flat 首版、物理缓存关闭及明确的 target interface 关闭设计选择，不宣称未实现的 compiler/arena/grouped 代码存在。

| 设计验收项 | 权威决策/证据 |
|---|---|
| 当前调用链及接入边界 | 第 1 节核对 buildPlan/逻辑 clone/optimizer/executeOnSnapshot/visitOperator/输出；保留前端与统一 evaluator，新增 IR 后 query-local 绑定边界 |
| 目标接口与具体字段 | 第 2/4 节及 JSON 决定 program/slot/operator/expression/pipeline、参数、property、batch header/selection/类型/动态 owner 的宽度与约束；现有 BoundSlot/BoundProperty 类型可编译验证，未实施容器明确标为 target |
| 编译与语义 | 第 3/5 节明确作用域/导入/遮蔽、projected-column 优先、lazy 错误、参数/batch type guard、混合类型回退、NULL/missing/浮点/溢出/空输入及算子算法；不复制第二语义实现 |
| 数据/schema/cache 身份 | 第 2/3 节；认证的双目录 observation、不同 overlay view/revision；初版不缓存物理计划；data pin/fence、能力、语义/参数 profile 均不被 schema observation 替代 |
| 所有权/预算/借用 | 第 4/7 节；固定列/PMR 容量与重叠、动态 arena/显式 legacy owner、batch owner/incarnation、最终拥有化、task 与 query shared 销毁次序；A6 的分配前完整准入仍是实施依赖 |
| 图中间表示 | 第 6 节；flat 首版、grouped offsets/相关归属/重复次数/NULL 扩展、路径与顺序、consumer 展开表及代表性准入实验；不强制采用未验证因子化或偷换 Cartesian 语义 |
| 执行状态/生命周期 | 第 7 节及现有 shared/task 类型；不可变 program、query shared、task、blocking merge、output 分开；单线程明确，O7 并行前的协调/错误/顺序/pin 责任具体 |
| 失败、恢复与回收 | 第 7 节；预算/取消/损坏/绑定冲突不吞错、不重放已消费输出；只有内存表示，无持久格式/WAL/retirement 新增，pin 及 A6 文件内 spill 的边界明确 |
| 迁移/替换条件 | 第 8 节列出现存适配位置、owner/语义职责及逐个移除条件；未迁移 operator 不计入 S2 完成，不绕回未声明、不计费的行 |
| 测试清单及当前证据 | 第 9 节区分当前组件与未来 compiler/arena/grouped/并行测试；真实 schema/property/overlay 及列/槽位/类型/预算/嵌套执行证明已实施边界 |
| 性能假设与反证 | 第 9 节；少行数编译成本、动态复制/转换/分组成本、资源及语义反证明确；最终同条件 control/current/Kuzu 与原第 6 节门槛，未运行迭代 benchmark |

当前源码必要模块：127 项 A1 组件测试 + 103 项 schema/property/overlay 测试，交集 23 项（property table 11、slot registry 12），合计 **207 项不同测试通过、0 失败**。包含真实新 catalog/旧 pin、staged 无 token 字段、独立数据库/reopen/DDL/overlay、所有实际 allocator/取消构造点、定长矩形失败回滚、选列/数值/嵌套输入及预算恢复。日志/链接命令/源码 hash 位于 `benchmarks/architecture-design/results/20261014-a1-design-contract/`。没有全量单元测试或性能基准，没有跨平台构建声明。

以上验收的是完整设计决定及其已实施基础，不替代 S2.1–S2.5、A6、O7 或最终性能。动态 arena、通用物理容器/编译器、grouped prototype、全算子迁移仍明确未实施；后续若不通过其准入门槛，不得把相关项改成完成。S1.1 的“明确后续实验”不把未来实验成功假定为事实。

固定 34 项新增 S1.1：S0 1/4（25%）；S1 3/7（42.9%）；S2 0/5；S3 1/5（20%）；S4 0/4；S5 0/5；S6 0/4。总体 **5/34（14.7%）**。当前 JSON 与此记录同步，正式 optimization.mdx 历史文本不改。进度增长后的代码、双语合同与证据按现有授权提交、推送；完整优化目标仍在推进。
