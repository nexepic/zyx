# A2 新建实体的组合属性补丁

日期：2026-10-12。组合 adapter 新增 created Node/Edge ordinal 支持。目标去重键包含 entity kind、reference kind、value，防止持久 ID 与同数值 ordinal 冲突。创建引用必须 kind 对应且 ordinal 在 companion batch 内，零 ordinal 合法；其他非法引用在任何读取或暂存前拒绝。单独 patch-only 接口仍无法提供创建引用。

创建补丁的源为同批次初始 create 属性，读取前计入 entry 工作上限，归一化成针对 created reference 的完整 replacement，交给既有后端在一次发布中处理。仅当补丁确实需要读取 persisted 源时才要求 snapshot shaped admission；输入创建载荷仍 caller-owned，source bytes 回调不冒充其容量计费。完整 replacement 与 patch 目标重叠对持久和创建引用均拒绝，不隐式定义两种请求的先后顺序。

新增三项测试：持久 Node ID 1 与 created Node ordinal 1 同批次分别修改，并创建 Edge 0 后删除旧字段添加新字段；最后 ordinal 越界不改变 overlay fence 或旧实体；创建完整 replacement 与 patch 重叠不暂存。严格构建成功，先定向十二项通过，补齐第三项后重新构建，最终显式选定模块共 708 项、33 套件全部通过。证据含源码 SHA-256、选定模块配置、最终构建/模块日志与 JSON。仅 macOS 验证。

尚未让 CREATE/MERGE/FOREACH normalizer 直接输出创建 patch，也未完成容量 owner、原生局部写入与 A3 布局生命周期。本轮仅完成真实组合 API 的引用支持与失败验证，不宣称整个 A2 验收完成。未跑 benchmark、全量单测、提交或推送，未更新正式文档或英文记录。

进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
