# A2 原生字段解析的取消传播

日期：2026-10-12。

原生 resolveProperties 过去只依赖执行入口和最终发布前的取消检查，较大的字段集合可能继续排序、解析和复制。现将 LogicalGraphMutationReadControl.checkpoint 显式传到所有调用：Node 创建、完整属性替换及 Edge 创建。描述符填充、按名称排序、键校验、解析前后、按 token 排序及 token 别名检查均可取消。Edge 创建前的 validatePropertyKeysAndTokens 同步接入取消，避免在真正物化前继续扫描整个输入。

新增测试通过首字段 token resolver 触发取消。后续字段若被解析将抛出不同异常；测试验证取消异常、仅一次解析、generation 不变和调用方乱序字段/值不被修改。它证明同步取消检查在解析链路生效，不证明大型单值复制中途可以抢占，也不覆盖整个原生 executor 的所有工作循环。

严格模块构建成功。原生 mutation、标量索引与约束三个模块合计 68 项测试通过。证据位于 results/20261012-a2-native-property-cancellation，含日志、JSON 和源码 hash。测试文件 1495 行，未超过 2000 行限制。仅 macOS 验证；未执行全量单元测试或 benchmark，未提交/推送，未更新正式文档或英文记录。

仍需完成原生字段 patch 消费、统一分配预算及 A3 布局生命周期；本轮没有新增完整验收项。进度：S0 25%，S1–S6 0%，总体 1/34（2.9%）。
