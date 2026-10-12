# A3 稀疏组 descriptor 候选 codec

日期：2026-10-12。仅实验格式，不修改生产 codec 或文件版本。

旧 NativeColumnImageStore 只承载早期 TypedPropertyBlock，不能直接用于当前稀疏列目录。新增 SparseGroupDescriptor 独立编码 owner 槽位、字段目录与 token 对应 LargeValueRef。

字节合同：24 字节小端 header，offset 0 magic 0x4753505A，4 version U16=1，6 reserved U16=0，8 字段目录长度 U32，12 列数 U32，16 owner 槽位长度 U32=现有 WIRE_BYTES，20 CRC U32（计算时置零）。随后固定 owner 槽位、完整字段目录，再逐项 token U32、reference 长度 U32、既有 LargeValueRefCodec 字节。没有修改既有引用格式。列引用种类为 BYTES，media type 为 application/vnd.zyx.lab.sparse-property-column，逻辑长度限制 48–560 字节。

编码和解码要求字段/字节上限、严格正数递增 token、固定槽位长度、身份与 CRC 正确，目录字段与引用集合恰好覆盖，无遗漏或无用列。引用长度在 decode 前检查；拒绝尾随字节。完整 descriptor 必须由未来原生 root 认证并由当前快照权威路由选择；传入旧 descriptor birth generation 本身不能证明它是当前版本。

测试覆盖往返、遗漏引用、错误 generation、字节上限、CRC 损坏、重算 CRC 的保留位错误与删去全部引用。初始测试引用漏填 content_hash，被已有引用 codec 正确拒绝；已补有效 hash，未放宽源代码。五个限定模块共 62 项通过，证据在同名 results 目录。

尚未读取子列验证内容 hash/codec/presence，因此此结果不是完整组验证 proof。没有 native publication、权威图路由、retirement 或 crash/reopen 证据。目录与引用覆盖校验仍遍历 owner/字段，需计入实际成本。下一步接入原生持久载体及生命周期验证。

进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
