### 2026-10-20 R3.2／R4.3：完整已分配池核对与实际字节反例

证据：`benchmarks/architecture-design/decisions/20261020-r32-full-allocation-census/`。新增只读、有额度及独立工作上限的 allocation census。沿认证 allocator radix 枚举全部已分配位，含物理文件范围外或无 Catalog owner 的位；跨 logical bootstrap／旧快照保持实际路由身份，页／visitor 上限返回未完成。与当前 Catalog、两类 Retire Queue 及不进普通 Catalog 的应急元数据池核对，重复物理归属／已提交池丢失 allocation 属于错误；不把损坏、取消或额度拒绝当零成本。

按完整128KiB池计入实体／属性／hybrid map／查询索引／大值／vector／State／metadata，包含未用帧与活池内旧版本。描述符、Catalog、allocator、Page Directory／metadata route 和退役 framing 一并包含在完整 metadata 池内，不重复加 payload。退役 Catalog 池和搬迁源另计；无归属位／private scratch 保持未知，不能授权转换。已分配字节与物理文件 high-water 分开，当前 generation／I/O epoch 变动时拒绝观测，饱和 epoch 无法证明稳定性时返回未知。

同一原生格式、64个owner各16个int64字段的实际例子：ROW稳态655360B（640KiB），候选1179648B（1152KiB），另保留131072B退役池；已分配峰值1310720B，文件high-water6029312B。该形状没有满足至少省10%的字节门禁。使用相同Property Store核对两个布局的值与旧pin，并核对完整分配分类；这只是字节／机制正确性证据，不是正式性能或完整形状矩阵，也不是完整查询执行器对照。

六项census测试和25项allocator模块定向检查通过。最终仅运行36个相关模块：默认527通过；候选775通过、1个条件跳过。没有全量单测或性能窗口。完整转换前估计器、同执行器查询对照、其余生命周期与R5／R7继续未完成。**总验收仍13/24（54.2%），不提交、不推送。**
