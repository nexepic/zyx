# Streamed schema direction validation

目录流式读取编码定义，保留一片叶与路由及一个输出记录，避免先收集全量编码 key。方向验证每批 64 个定义，预付 key、pair/keys/value 数组、前缀重叠和调试代理上界，全部名称/语义映射仍逐项验证。跨批重名通过规范键唯一映射到 ID 的核对拒绝；旧快照保持有效。4 个定义入口无显式调用方额度时选择快照 CACHE 所有者，正常原生调用也保留真实计费；已有显式额度优先。94 项定向测试通过，含跨批损坏、默认快照归属、额度精确归还和字符串容量/计数赋值的拒绝保护。原始 B+Tree 叶/路由解码等临时容器继续治理。

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; prior885-case matrix belongs to the preceding frozen source, not this phase. No Windows/macOS runtime, RSS or performance claim.
