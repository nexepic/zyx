# R3.2：整 owner 删除的私有生命周期

仅推进原固定五项中的 R3.2。阶段验收仍13/24（54.2%），没有提交/推送，没有全量单测或新增性能窗口。

`HybridPropertyDemotionPreparation::prepareOwnerDeletion` 在精确来源 pin/MetadataWriteBatch 下完整枚举最多64个同组 live owner 的字段，拒绝重复、已死、跨组或缺失来源；本次字段工作量最多4096。每个字段列的多 owner 删除先合并再判断48成员阈值，随后使用已有共享 COW 合并器。普通部分列构造仍拒绝 clear_live；只有完整 owner 枚举工厂能进入内部协调路线。

合并后的 group memberships/columns 再扣除删除的已有 extension，并将所有被删 owner 的 live 位清除一次。保留合格列，降级不再合格的列，移除旧 extension 物理 slot 与 owner/extension 目录键；最终一个 owner 消失时删除 group slot 和目录键，不编码非法 live=0 的 group。仅有 extension 或没有受影响列的 owner 使用同一已认证来源页副本。跨字段生成的新 extension 共用页面；已有同页图像继续复用，不再复制一次来源页。返回值仅包含私有图像与最终目录变更。

当前 group.live 是权威；未参与删除的列可保留旧 live 快照，只要求 presence 不含已死 owner。因此无需重写 owner 没有参与的列，测试验证该列原始字节完全不变。删除共享 LargeValue 的一个 extension 不退役其对象，另一个 owner 的原始引用和 manifest 仍可访问。全局 ROW/native 引用合并及安全退休仍由未来发布协调器负责。

移除前一检查点对整个 group 总 memberships<=4096 的限制。R3.1 明确4096为一次转换计划的上限，不是布局终生容量；本工厂对本次枚举字段工作量限额，普通混合字段准备按最多64列，内部 owner 删除按最多85个合并列进行有界私有准备。更宽的删除/大账户发布仍需后续流式协调，不据此宣称支持任意宽事务。

相关模块默认4/4、实验56/56通过，覆盖49→48列保留、48→47列降级、多字段与NULL、所有owner删除后组消失、extension-only/shared LargeValue、无关列字节不变、早/中/晚取消、额度退款、无效owner与独立重试。来源文件大小/epoch、真实 Entity/Property 来源及发布根不变。测试文件仍低于2000行。修复了上一检查点新增测试借用临时 readRecord 缓冲区的问题，明确保留缓冲区至扩展解码完成；测试准入分别保留 extension 与 manifest 的账户。初轮 fixture auto 混合类型声明的编译失败保留，已修复。原始日志、JSON、二进制与源码哈希随附。只验证 Linux x86_64。

剩余：已有 extension set/remove 与跨组/其他最终字段修改协调；将实际 Entity Map 删除与这些字段/目录变更在同一事务发布；物理 COW登记、Page Directory/Catalog/allocator/描述符/退休原子提交；完整恢复、GC/搬迁复用与同执行器ROW成本对照。生产非空格式与普通写路由保护仍在，R3.2/R3.3未验收；R5三项仍按原合同继续。
