# R3.2 有界字段转换与快照合同

父提交 `ba8cf295`，本目录为实现检查点，不是预登记性能测试。R3.2 继续进行中，总验收仍为 13/24；未安装原生 hybrid 文件 writer、格式或图路由。

`PropertySubblockFieldTransition.hpp` 实现对一个现有已准入列的纯有界转换：返回 no-change、单列替换或仅该字段最多 64 单元的降级。不接受 extension/ROW 源进行自动提升。存储 NULL 保留 presence、payload 归零，REMOVE 清 presence；类型变化或 48→47 槽使目标字段降级。降级结果把未使用的 Column 成员置为非法，防止错误分支将伪造的标量零编码发布。opaque extension 标记指调用方仍持有的 canonical 输入，不是编码值或新造大值引用。内核不访问其他字段/大值体。

`validateColumnSnapshot` 落实 R3.1 的存活/代际定案：当前组 live 为权威，历史列 live 仅可在缺失单元上不同。要求 presence 属于当前 live，列代际 <= 组代际 <= 描述符代际 <= 可见代际，匹配 group/owner/database 且必要命名空间根存在。期望 token/原生 page/slot 仍由原生读取方另行认证。创建/删除不含此字段的 owner 因此无需改写无关列；死 owner 仍有 present 值则拒绝。不扩展现有 EntityPropertyKey ID 范围。

no-op 与 ROW 的真实 PropertyValue 比较一致：int/double 不混同，正负零相等，NaN 即使 payload 相同也不相等；实际替换编码仍保留 IEEE 原始位。审阅发现初版按位 no-op 会不兼容，修正内核及预期，并新增 12 种基本值/missing/NULL 的全部 144 对与真实 optional<PropertyValue> 比较。保留 `before-semantic-review-32.json` 中间结果，不能把它作为旧比较合同正确的证据。

纯内核进入前检查调用方给定边界至少容纳 sizeof(FieldTransition)，并检查原生记录可用边界；暂存/记录超限时不修改 const 输入。该检查不会实际预约账户，原生调用者仍须先按 R6 实际预约并持有到 plan 释放，计费 canonical extension 输入和 map/allocator 计划，再原子发布归属/索引/描述符/fence。真实变化的代际必须推进，no-op 不生成替换。独立内核尚未实现完整反向转换、全局 integrity 或字段退役。

最终 33 项通过：原有 codec/门禁 20 项，转换/快照/ROW 比较 13 项。ASan/UBSan 在 ASAN_OPTIONS=detect_leaks=0、UBSAN_OPTIONS=halt_on_error=1 下同样 33 项通过；不宣称当前环境不支持的泄漏检测通过。首轮 CTest 为 29 项，其中一项测试体异常：fixture 第二次真实修改仍传入已使用的代际 6。保留源码拒绝行为，把 fixture 改为 7，首轮证据见 `first-29-failure.log`。实际仅 Linux x86_64；未跑全量单测或正式性能/Kuzu 窗口。

`20261019-r3-codec-five` 的独立 CMake/runner 现包含两个 suite，并添加根头文件用于真实 PropertyValue 比较；其原先 20 项证据仍属于记录的历史源码提交。本目录 final JSON 和源码 hash 标识扩展后的检查点。下一步仍是原生 root/descriptor/page-kind/fence/读写/完整性/搬迁/退役原子接入，以及 R3.1 完整生命周期/成本/同执行器矩阵，之后才能验收 R3.2、进入 R3.3/R7 准入。
