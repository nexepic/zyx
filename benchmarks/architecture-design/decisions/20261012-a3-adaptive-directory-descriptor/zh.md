# A3 自适应目录接入实验 descriptor

日期：2026-10-12。仅修改实验格式，不采用已拒绝的 LargeValue 载体为生产布局。

新增 encodeAdaptive/decodeAdaptive。9 owner 及以下使用原成员目录，10 owner 及以上使用 SHA-256 绑定的紧凑槽位计数目录；9 owner 的固定成本相等，选择原格式。按 tag 明确解码，未知 tag 拒绝。

实验 SparseGroupDescriptor writer 升级 outer version=2，允许两种经过验证的目录。reader 接受合法 v1（仅原目录）与 v2（两种），拒绝 v1 搭配紧凑目录，不通过静默尝试另一 codec 接受格式错误。新版子目录仍保留自己的版本与 owner 表绑定。

新增 1/9/10/64 owner 边界往返测试，验证编码选择、长度、所有 fieldless owner 与 v1 兼容/拒绝。原生 writer、reader、故障发布与恢复回归保持通过，五个限定模块共 68 项通过。证据在同名 results 目录。

更新结构计数：共享字段候选 descriptor 1822 + payload 560 = 2382 字节，当前 owner 根3456；独有字段 descriptor11272 + payload3584 =14856，约当前根4.30倍。只比较 codec 字节，不含完整目录/manifest/索引/页填充。紧凑目录消除了共享字段场景的本项空间反证，但稀疏载体仍不满足直接生产采用条件，上一轮拒绝决定仍有效。

后续继续共享页/紧凑子块定位、异构与稀疏回退、权威路由及回收；预算和取消也尚未全部闭合。进度保持 S0 25%、S1 14.3%、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未修改正式数据库文档。
