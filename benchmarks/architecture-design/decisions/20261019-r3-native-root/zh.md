# R3.2 隔离原生描述符根检查点

本检查点实现空描述符的原生元数据生命周期，不是完整属性子块交付。固定五项仍为 R3.2/R3.3/R5.1/R5.2/R5.3，完成数 0；主文档总计仍为 13/24。代码身份、源文件 hash、变更 patch、构建/二进制身份和原始测试结果均在本目录。

| 构建 | Superblock/header/profile | 描述符 |
|---|---|---|
| 默认，实验选项 OFF | 31 / 336 / 5 | 不允许持久化 present 根，保持 ROW |
| 隔离实验 ON | 32 / 352 / 6 | 仅允许八个业务根均为零的空描述符 |

实验选项为 `ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT`，默认关闭。新增根位于 Superblock offset 336..351，使用现有 physical-reference packing；emergency reserve 在实验格式从 352 开始。两种构建拒绝对方文件，不做迁移。完整布局见 R3.1 的 `r3-layout-zh.md`。没有向产品宣称已支持非空 hybrid 属性，也未增加新的数据 pool/page-owner kind。

描述符位于现有 metadata pool 的独立物理 frame，不伪装 State/radix、不分配 routed MetadataPageId、不加入十二根 logical bootstrap 列表。元数据恢复、只读发现、可达性、旧 pin 保护、保守目录搬迁和清除回收显式识别该根。局部搬迁遇到额外物理 frame 会保留既有 conservative fallback。搬迁更换 physical generation/LSN，保留 semantic generation。普通更新仍按原 data fence → inactive A/B → superblock fence 发布。

图 fence 新增第八根描述符（默认文件此根为零），两种格式使用不同 format token。ID-only 判定和盲邻接 rebase 不得忽略该根变化。写入方在分配 frame 前拒绝非空业务根；reader、open、取得新 snapshot 和完整性扫描也会认证并拒绝当前未支持的非空路由。有效校验和不能替代 database/physical generation/LSN 身份校验。新 snapshot 验证失败时，刚取得的 pin 会归还。描述符读取使用文件 view 固定的 database ID，避免读取 writer 正在 rebind 的 routing Superblock 身份字段。

PROFILE 的固定计费 registry 新增 `native_graph.read.metadata_decodes.hybrid_descriptor`，含义为当前线程累计的描述符解码尝试数。registry 与结果 staging 上界从实际条目数派生，没有提高资源上限。描述符目前使用固定 4 KiB 编解码缓冲；完整成本应计算该 frame、真实 128 KiB pool 保留容量和旧 pin，不能把 192/224 字节当成实际分配。没有完整原生成本估计器，不能据此准入转换。

最终检查分两部分，各部分是明确 filter，未执行全量单测：默认构建十三个必要模块 276 项通过，另对根身份/fence/PROFILE 的十二项检查通过，合并后 285 个不同用例最新通过。实验构建十三模块 285 项（284 通过、1 项默认格式专用测试明确跳过），另二十一项根身份/fence/PROFILE 检查（20 通过、同一默认测试跳过）；合并后 294 个不同用例，293 通过、1 跳过。重复用例只计一次，不是一次 285/294 项运行。原始文件为 `*-lifecycle-final.json` 和 `*-root-profile-final.json`，对应 scope 文件记录编译模块、实际 filter、构建开关和链接命令。两个构建的实际 PROFILE format token 不同，由 `verify-evidence.py` 检查。

检查覆盖旧 pin/重开/只读、清除后回收、描述符搬迁及整目录保守 clone；三种 A/B failpoint；真实 native 文件句柄下的 descriptor ENOSPC、data fence 失败和最终 root fence 不确定结果；合法校验和下错误 database/generation/未支持路由；新快照失败不泄露 pin；正常 ROW/Node/Edge/Vector/索引/元数据基础模块保护，以及 PROFILE 预算、输出、只读和异常清理。故障后恢复恰好一个完整状态，可独立重试，不把不确定 ack 当成一定回滚。仅实际验证 Linux x86_64，未宣称三平台、完整绑定矩阵、完整规模或正式性能通过。

复现可使用 `run-necessary-modules.py <build-dir> <output-dir>`，以及 `run-final-identity-check.py`（根身份、已改 fence 用例和八项 PROFILE 保护）。`run-fence-regression.py` 可仅运行改动的 fence 用例。runner 依赖当前 Linux cloud 工具链/gtest 路径；`ZYX_SCOPED_TEST_JOBS` 只控制编译并发，不改变数据库运行资源。源证据核验执行 `python3 benchmarks/architecture-design/decisions/20261019-r3-native-root/verify-evidence.py`。本轮保留私有构建与原始中间结果，没有清理用户数据或覆盖正式控制二进制。

下一步是实际字段 pool/page-kind、目录 locator 的同发布身份分配、最终八树根绑定及属性生产者/写入路由。非空根保护必须与完整 reader/writer/integrity/large-value 精确归属/搬迁/退役一起替换，然后完成 R3.1 生命周期矩阵、原生完整成本和同 R1 执行器 ROW 对照，才能验收 R3.2。R3.3 与 R5 继续按原文档执行；性能/Kuzu 仅在最终 R7 统一窗口运行。当前不宣称提速。
