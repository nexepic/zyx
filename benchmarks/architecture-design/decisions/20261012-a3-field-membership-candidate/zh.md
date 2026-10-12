# A3 实际字段枚举候选

日期：2026-10-12。候选原型，未生产接入，未变更持久格式。

现有 TypedPropertyBlock 与 PropertyBlockOwnerSlots 有列载荷和有代际的 owner routing，但不能据此宣布具有完整生命周期。本轮增加 PropertyBlockFieldMembership，按 owner 实际字段维护有序 token 列表；与实体插入顺序、标签和全库 token 最大值无关，避免完整实体读取时枚举全库字段。

使用旧实验槽位容量 64 作为候选载体参数，不作为最终组大小选择。构造前验证 owner 集与累计字段工作上限，拒绝零上限、超限、零 token 和重复/乱序 token。每个目录绑定完整 owner route，读取既验证当前 slots.resolve，又验证目录自己的 route，防止新 generation 复用同槽位后读取旧目录。目录不复制字段值；旧 slot/目录对象作为不可变快照仍可读。

空间为每组固定 64 个成员描述符加实际字段 token capacity，避免 N×K 稠密字段矩阵；固定描述符对低占用组可能昂贵，尚未证明候选优于旧布局。metadataCapacityBytes 只报告对象及 vector 元素容量，排除 allocator 头部/RSS，不是持久编码尺寸或完整元数据成本。

三项新测试覆盖交错插入/稀疏高 token、槽位复用及旧快照、无效/超限目录；连同两项旧布局编码对照共 5 项通过，严格编译警告作为错误。证据位于 results/20261012-a3-field-membership-candidate，含日志、JSON 和源码 hash。代码和注释为英文，测试由仓库 GLOB 发现并纳入 selected module。

持久 codec、页布局、实体目录接入、字段块权威归属、跨结构原子发布、崩溃恢复、删除/分裂/回收及预算仍未完成；该目录不是第二份权威属性副本，也不是获准生产布局。后续需要给出枚举元数据与列块共同 COW 的完整协议并实测真实页数/字节。未运行 benchmark、全量测试，未提交/推送，未修改正式文档或英文记录。

进度：S0 25%，S1 14.3%，S2–S6 0%，总体 2/34（5.9%）。
