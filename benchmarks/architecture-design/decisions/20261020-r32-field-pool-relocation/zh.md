### 2026-10-20 R3.2：字段池有界搬迁、退休与物理槽复用

证据：`benchmarks/architecture-design/decisions/20261020-r32-field-pool-relocation/`。

显式元数据维护入口按数据库/generation/描述符/source SegmentId精确fence，每次只检查一个来源池至多31个frame。对照当前Page Directory，只复制仍路由到该池的原生字段页；存在当前ROW或退休frame则拒绝。先写新的字段池，再在一个A/B发布中更新Page Directory、登记旧池退休和Retire Queue，保留PageId、逻辑字段/目录及描述符原根。来源池字节不修改，也不为此扫描全Catalog或整库字段。

旧pin继续读取原始字段并阻止释放；pin排空后通过已有受控整池回收清理旧身份，再以新逻辑SegmentId复用其物理槽。片段池仅搬当前frame，不复制已失去路由的旧frame。共享账户上限仍1 MiB；目的槽不足、额度、取消和错误fence在发布前明确拒绝。

16个必要模块：默认303通过；候选459通过、1个条件跳过。覆盖宽组多页、更新后的碎片池、物理路由与逻辑根不变、Node/Edge、只读重开、旧pin/释放/复用、A/B恢复、预算/取消前后退款重试、无可用目的槽和来源pool损坏。首次复用夹具误用了短页readAt，已按接口改为整页读取后解码envelope；失败日志保留。测试公共fixture已提取，单个测试文件保持2000行以内。没有全量单测或新增性能窗口。

目录树搬迁/混合物理代际后写入、通用大值/引用回收、整owner/Entity Map协调、多组及最终完整成本/同执行器ROW对照仍待完成。**无新增阶段验收，总计13/24（54.2%）；不提交、不推送。**
