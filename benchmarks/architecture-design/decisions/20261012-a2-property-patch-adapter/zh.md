# A2 按键请求与旧布局原子适配入口

日期：2026-10-12。任务：S1.2/S3.1 局部实现。状态：LogicalGraphWriteBackend 新增真实可调用的按键属性修改入口，当前源码 31 个相关模块套件共 511 项通过。Cypher 直接输出 patch、原生局部属性写入、完整容量计费及创建/替换混合批次仍未接入；完整 A2 未验收。

## 接口及当前调用链

LogicalGraphPropertyPatchRequest 使用现有实体 kind/reference/value，sets 持有非 NULL 的 LogicalGraphWriteProperty，removals 持有字段名。当前入口 executeLogicalGraphPropertyPatches 接受 caller-owned snapshot、patch span、正数 entry 上限、显式规划 fence 和同步 read controls。当前仅支持有非零 ID 的 persisted 引用；同一批次不得重复目标，Node 与 Edge 身份空间分开。新增结构不改变旧 replacement 的含义，也没有给 LogicalGraphMutationBatch 增加会被旧消费者忽略的字段。

所有请求先验证：kind/reference 合法，key 非空且两列表各自无重复、互不重叠，sets 无 NULL。NULL assignment 应由调用层显式转为 removal。本轮没有新增 Cypher 语法或自动改变已有调用方。目标数、输入操作数及累计源属性数都有有限上限；这不等于容器/动态载荷的完整字节预算，后者仍须继续接入 A6 owner。

适配器按给定 snapshot 读取完整原属性，检验返回身份与源 key，保留未指定字段，应用删除与 set，按名字顺序生成现有 replacement 请求。全部归一化成功之后才调用 executeLogicalGraphMutationBatchWithReadControl 一次，复用 overlay 与原生后端的原子发布/约束/索引/恢复合同。source/control 失败、缺失目标、后续请求非法和分配失败均不能提前暂存前面的实体。源及归一化数据由该调用拥有，snapshot 必须覆盖调用生命周期。

该入口是旧布局过渡适配器，仍读取完整集合并生成完整替换，不宣称属性已经局部写入或减少全部复制。现有 Cypher 写路径尚未调用此入口；后续需要让语句 normalizer 产生最终按键意图，并允许原生实现消费增量而无需完整替换。不得在只有该适配器时删除通用 replacement 语义。

## Fence、准入和错误

规划 fence 是必传值，验证其有效且等于源 snapshot 当前 fence，随后将已取得的 fence 交给底层原子发布再次验证。初版仅在调用时获取源句柄 fence，测试发现 overlay snapshot 是活视图，旧句柄会反映新 revision，不能代表旧规划身份；已修正生产接口为显式规划 fence，而非让测试接受过期意图。

同步 checkpoint 覆盖请求验证、源准入、归一化以及底层暂存前的检查。要求 property_admission 时，snapshot 必须支持 shaped admission，否则直接拒绝，不能退回未经准入的完整读取。每个实体的绝对源字节在本批次累计，累计溢出明确失败；这些是已定义的编码源 scratch 估计，不是解码容量/RSS，也不为新值载荷提供完整字节上限。源结构损坏、身份不匹配和 invalid request 分开报错，取消异常原样传播；不进行请求重试。

当前尚未移除空 patch/值未变化产生的完整替换请求，底层继续按现行协议处理；该行为不应被宣称为完整的无变化请求折叠。创建 ordinal、同批次完整替换与 patch 混用、map 语义以及查询级准确容量准入仍是接下来的接入合同。

## 模块测试与证据

新增 6 项测试：连续 patch 的 read-your-writes 与未指定字段保留；最后请求非法时整批不暂存；entry 超限与旧规划 fence；取消和 read-only；节点/关系一次暂存及缺失后续目标不影响前面节点；源 admission 不支持或拒绝时不暂存。修正后的最终模块结果也覆盖不支持 shaped admission 的新增断言。原初 stale 测试暴露上述活视图 fence 问题；接口更新时测试机械替换留下两个未定义变量，已修复并重新构建，未改动原有语义断言。

当前源码构建后，511 项选定模块测试全部通过；并非全量库测试。验证为 macOS 本机，不声明 Linux/Windows 已构建验证。新增代码/注释/错误字符串为英文。未安装依赖；本轮无 Docker、临时持久数据库或文件格式变更，构建缓存继续保留供后续模块测试。

证据位于 benchmarks/architecture-design/results/20261012-a2-property-patch-adapter/，含源码 hash、模块 CMake 配置和日志/JSON。focused 为第一次成功的 6 项定向测试，modules 为最新新增断言后的 511 项通过结果。复现：cmake --build /private/tmp/zyx-a1-module-build --target zyx_binding_module_tests -j 4，随后执行该选定模块目标。

未运行 benchmark、全量单元测试，未提交或推送，未更新正式数据库文档或英文历史记录。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。不将请求类型和适配入口完成等同于 A2 全部优化完成。
