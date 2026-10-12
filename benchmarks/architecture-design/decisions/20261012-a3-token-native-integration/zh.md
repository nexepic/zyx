# A3 原生 token 路径接入

日期：2026-10-12。

PageBackedGraphReadAdapter 新增 readEntityPropertyTokens，绑定快照会话并先验证节点记录或边拓扑存在性，再调用已认证存储目录读取。A3 buildFromNativeSnapshot 改为直接消费 token，移除临时 PropertyCollectionRoot/PropertyValue 集合及重复 nodePropertyCount 读取。按实际 owner 建立目录，保留快照身份、slot generation 和排序语义。输出 token 分配前检查累计字段数量；输出及 LargeValue 目录准入均在前后检查取消状态。

真实 LargeValue 测试增加适配器读取、原型目录构建与字段上限拒绝；已有节点、边、不存在实体、预算和取消回归继续执行。四个限定模块共 25 项通过。新增断言起初在预期异常后错误检查准入回调归零，已移至成功读取后；异常准入清理责任保持上一轮约定，没有改变生产行为来迎合断言。

当前 token 入口尚不共享 property cursor，元数据目录和输出 vector 容量仍未归入统一查询预算。LargeValue 认证可能读取包含目录的完整 chunk，不能宣称物理读字节仅为目录长度。目录校验循环尚无细粒度取消 checkpoint。此路径用于 A3 候选目录来源，不意味着生产属性分块格式、页生命周期、回收及恢复完成。后续继续补齐这些合同与准入证据。

证据保存于同名 results 目录，包括 Werror 构建、测试 JSON 和源码 hash。进度保持 S0 1/4（25%）、S1 1/7（14.3%）、S2–S6 0%、总体 2/34（5.9%）。未运行全量测试或 benchmark，未提交或推送，未更新正式数据库文档。
