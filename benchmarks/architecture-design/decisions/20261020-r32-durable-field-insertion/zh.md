### 2026-10-20 R3.2：缺失字段插入与既有列成员新增

证据：`benchmarks/architecture-design/decisions/20261020-r32-durable-field-insertion/`。

显式生命周期入口支持存活组内缺失标量/NULL字段插入。先认证 owner membership 不存在，且没有冲突 extension locator；已有同token列时认证真实列/缺失presence，生成新增成员delta。与删除/更新合并后再判断48槽阈值，兼容值直接扩列，异型值将全部最终成员降级。没有列时创建独立extension，空存活组可首次建立共享owner/extension目录；缺失字段删除为无写盘no-op。默认兼容列入口仍不接受新增字段。

正负membership效果共同计算，不把4096转换工作上限当成group终生容量。新增owner来源、字段页/目录、描述符和整池退休同次发布；存活位、主Edge及旧pin保持有效。15个相关模块默认302通过，候选447通过、1个条件跳过；新增覆盖49号无属性owner加入已有列、同字段一删一增仍保留48槽列、NULL、新token扩展、异型新增降级、空组首次建图和逆转换。未跑全量单测或性能窗口。

扩展晋升、通用值/大值引用生命周期、整owner/Entity Map联动、多组、搬迁和最终成本/ROW对照仍未完成。**无新增阶段验收，13/24（54.2%）；不提交、不推送。**
