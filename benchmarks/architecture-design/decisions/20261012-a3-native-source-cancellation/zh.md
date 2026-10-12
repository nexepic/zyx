# A3 原生源读取的取消传播

日期：2026-10-12。

buildFromNativeSnapshot 增加同步 checkpoint，在入口、每 owner、实体记录/拓扑、字段计数、属性读取后及目录构造前检查。强制源 admission 外层包装在调用前后都检查，避免回调已触发取消却继续进入属性载荷复制。Node/Edge 共用该传播路径，取消异常不吞掉。

真实 native Node/Edge 用例扩展：首个 source admission 设置取消，随后 checkpoint 抛出，验证仅一次 admission、metadata generation 不变，原目录仍可读取。严格构建与相关四模块 22 项通过。证据位于 results/20261012-a3-native-source-cancellation，含日志、JSON 与源码 hash。

本轮没有使完整目录 token 复制/CRC/字段关系校验的每个循环都可取消，也不保证单次底层读取可抢占。原生源仍临时解码完整值，Edge 的字段数工作上限在读取后检查，统一预算/token-only 读取仍待实现。候选未生产接入，持久 descriptor、原子发布与回收未完成。

未运行 benchmark、全量测试，未提交或推送，未更新正式文档或英文记录。进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
