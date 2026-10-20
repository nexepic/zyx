# R3.2 私有共享字段页准备

本次为基于 `6a3fedf1` 的未提交工作区检查点。进度仍为 13/24（54.2%）；验收百分比未增长，因此不提交／推送。固定的 R3.2/R3.3/R5.1/R5.2/R5.3 均未完成。以源码 hash 和工作区补丁标识候选，不虚构提交身份。

`HybridPropertyPageImage` 将 group／column／extension payload 打包到现有中立 SlottedPage 图像，多组可以共页，不要求每 owner／每列独占页。类型化更新校验字段身份并保留无关槽位；验证私有 clone、字节重开、erase／reuse 和 compact 的稳定逻辑页／槽位／代数引用。这是私有图像，不是已发布池、真实图快照或格式／根切换。类不分配逻辑 ID、segment/page owner kind 或描述符根，绝不能作为普通 ROW 页持久化；非空描述符保护不变。

extension 物理目录已明确为固定宽度：12 字节 owner/token 有序前缀加 16 字节 locator，48 字节信封和变长内容留在共享字段页。`HybridPropertyDirectoryCodec` 同时提供原生 owner／column／group 键及 selector；保留位、非法原生 ID、no-slot 哨兵和零代数均拒绝。页 CRC 和 locator 合法不代表可解释为错误记录类型。固定 map 复用既有最终变更合并，不采纳会把完整字段内容带入内部 separator 的变长 key，避免降低扇出和复制大字段。新增定位／页读成本仍须由完整 R3.2／R7 对照验证和准入。

数据池物理 generation 不等于语义提交代数：只要满足 group／descriptor／snapshot 上界，语义代数 7 的列可位于物理代数 1 的池；描述符元数据帧另有物理发布规则。`decodePayload` 仅用于私有源身份检查，图读取的完整 column decode 继续校验组／描述符，并避免重复一遍 payload 校验。

`SlottedPage::readOwned` 和 `serializeOwned` 强制提供目标分配账户，实际复制由该资源准入，逃逸缓冲区持续持有它。原有 API 和字节格式不变。验证了配额拒绝／退款／重试、源不变及资源生命周期。固定 codec／compact scratch 仍须在后续有界发布协调器中明确准入；本次缓冲区检查不代表完整查询／RSS 或全部资源准入。

四个必要模块在默认格式 31 和隔离格式 32 构建中分别 48/48 通过：新增 page-image／目录、原生字段记录、extension 信封及既有 SlottedPage 模块；这是同一批 48 个用例的两种配置。仅运行 Linux x86_64 Debug＋coverage，未执行全量单测、正式计时、Kuzu、其他平台或生产准入。保留原始结果／范围、源码身份／补丁及二进制 hash。加载既有工具链后，用 `python3 run-necessary-modules.py BUILD_DIRECTORY OUTPUT_DIRECTORY` 复现；`verify-evidence.py` 检查当前源码 hash、原始范围／计数及派生进度。

后续须在同次发布中先分配元数据持有的 data ID，再准备最终 locator／map／描述符；接通 pool／owner kind 及全部原生读写路由；完成双向转换、降级／删除、索引／大值归属、完整性／搬迁／复用／退役／故障，以及完整分配成本估计与同执行器 ROW 对照。不能移除保护，也不能把私有打包计为完整 R3.2 生命周期。
