
### 2026-10-21 R3.2：新实体 Vector 与约束证据

证据：`benchmarks/architecture-design/decisions/20261021-r32-owner-derived-publication/`。生产源码与此前十模块检查点（候选219／默认100）一致；扩展后的生命周期模块候选56项通过，默认格式编译通过。新增检查覆盖原生字段降级与 Node Vector bootstrap 同批发布、超级块部分／完整写入恢复、Node／Edge UNIQUE 支持树、NOT NULL 与整数类型约束、批内／源库重复值拒绝，以及必填失败不留下新 Catalog 字段名。故障场景继续在旧 pin 保留时通过同句柄写入。定向正确性不等于性能证据；测试改用具名选项。混合／detach／跨组与完整准入仍待完成。**14/24（58.3%），验收无增长，不提交或推送。**
