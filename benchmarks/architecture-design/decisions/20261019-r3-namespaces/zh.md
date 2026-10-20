# R3.2 固定字段命名空间绑定

这是清单base上的未提交工作树检查点，不是已验收源码提交；验收仍13/24，未commit／push。累积源码补丁也包含此前原生转换／私有图像准备。

HybridPropertyIndexIds.hpp固定现有八个描述符ordinal，不改变顺序或磁盘宽度。Node owner／column／extension／group、Edge对应四项依序保留0x8000000000000030..37。审计未发现与schema definition的[2^62,2^63)范围、全局邻接...0002、System Catalog...0020、vector A501..A504 mapping前缀或LargeValue UINT64_MAX重叠。描述符codec仍仅表示PageId／物理池代际的完整成对引用，namespace认证属于外层协调器及实际路由reader。

普通图标量upsert、erase、按entries创建、按plan创建和drop在两种构建中均拒绝该保留范围。元数据在派生数据准备之前验证每个非零替换IndexId与描述符ordinal精确匹配，不能绑定普通索引或交换Node／Edge／字段角色。既有最终新／保留／缺失／退役根解析、空描述符发布、物理代际测试及全部A/B检查继续通过。未提交图发布fixture现使用合法的保留owner命名空间；缺失树用例改为请求正确但不存在的角色，在加强合同后保留原来的失败触发。非空描述符仍拒绝；marker树只是普通测试记录，不是已认证字段数据。

三个必要模块：默认42/42、实验52/52，无失败／跳过／禁用。原NativeGraphWritePlanner公开合同测试文件与base字节完全一致。新增用例覆盖精确ID／范围边界、40种保留ID标量暂存组合及准备前错误角色／普通索引拒绝。重编译核心依赖，但未执行全量单测或正式性能／Kuzu窗口。中英文单文件规范同步默认5／31／336、隔离6／32／352的profile／Superblock／header兼容矩阵、描述符／私有owner边界及拒绝保护；MDX编译和文档链接检查通过。

尚未安装普通图字段转换、物理字段池、最终map／描述符生命周期、全部属性读写／索引／overlay、精确LargeValue归属、完整integrity／恢复／退役／复用／搬迁、完整原生成本或ROW对照。保留命名空间身份不授予持久化或生产准入。固定R3.2／R3.3／R5.1／R5.2／R5.3仍未完成。
