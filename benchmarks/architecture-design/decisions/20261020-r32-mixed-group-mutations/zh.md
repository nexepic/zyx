# R3.2：同一 group 的混合字段变更

本检查点仅推进原固定五项中的 R3.2，不增加阶段验收：13/24（54.2%）。没有提交、推送或新增性能窗口，也没有运行全量单测。

`HybridPropertyDemotionPreparation` 现在接受同一来源 group 的最多64个最终列计划；同一列的多 owner 变更必须先经过 `mergeExistingColumnChanges`。单字段接口委托给同一实现。数据库、精确来源/下一代、group locator 与完整来源 group 字节必须一致；拒绝空、移动后计划、重复列及需要 whole-owner/live 协调的删除。来源成员数不超过4096。

兼容更新、仍合格的列删除和多个字段降级先合并最终 memberships/columns。按真实 PageId 去重，只复制一次来源页面；全部 group/column 来源认证成功后才修改私有图像。保留的列使用最终 group 校验，降级列被移除，其新 extension 跨所有降级字段共享页面；无关扩展字节不变。输出 owner/extension 变更按 key 排序，输出所有列删除及 owner 删除。额度只预约实际将生成的 extension 数量。全部 no-op 不产生图像或消耗私有 ID。

实际原生来源测试覆盖同页两个降级与第三列更新、删除后恰好48成员的保留列与其他字段降级、NULL、最终 extension 逐值解码、目录排序、来源字节不变、重复/空/移动后计划拒绝、额度退款及早/中/晚取消退款和重试。默认格式模块4/4通过；实验格式模块52/52通过。原始 JSON、构建/运行日志及源码/二进制哈希随附；初轮取消测试误用抽象 checkpoint、默认构建漏加实验测试条件的编译失败日志一并保留，均已修复。仅验证 Linux x86_64。

仍未完成：跨 group 最终协调、whole-owner/live 与已有 extension set/remove、物理位置登记/退休、Page Directory/Catalog/allocator/最终目录/描述符原子发布、完整恢复/GC/复用和同执行器 ROW 成本对照。私有图像不是成功的持久转换；非空格式与生产路由保护保持启用。R3.2/R3.3 不能据此验收，R5 三项仍按原合同继续。
