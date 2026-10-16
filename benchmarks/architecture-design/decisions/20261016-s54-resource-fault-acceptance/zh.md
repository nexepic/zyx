# S5.4：资源故障与清理验收

结论：S5.4 已验收。本项检验低内存、取消、磁盘不足、只读和崩溃后的行为；不将组件数量算成阶段数量，也不同时验收尚未完成的 S5.3、S5.5、S6.1 或 S6.3。文档允许按依赖跨阶段推进。

源码基准为 `05cba31b046cafc26775ecb1cec6dd80367b112f`。本轮只增加验收证据，没有修改运行路径。未接入原生路径的 OwnedFunction 原型不属于本项，也没有借此声称全部原生分配已预算化。

## 验收矩阵

| 条件 | 证据 |
|---|---|
| 低预算、并发总额和退款 | 查询/数据库/进程准入、跨别名共享、失败前无发布、结果保留、销毁后退款 |
| 取消和提前停止 | 排序 run 封存后取消、首次发布前取消、输出提前结束、DDL 各检查点回滚 |
| 磁盘不足和不确定 I/O | 真实 ENOSPC/Windows 对应错误注入、数据/根短写、最后同步失败；旧图保持可读，写入封锁后重开恢复 |
| 逻辑只读与物理只读 | 可写文件上的只读查询实际落盘排序；物理只读拒绝 scratch，孤儿检查不写文件 |
| 单文件 scratch 与用户提交交错 | 用户根不丢失、图 fence 不变、私有句柄隔离、长图快照不妨碍私有空间释放 |
| 损坏 | 损坏 payload/目录不能触发用户页回收或错误退款；不能返回部分成功结果 |
| 分区哈希结果 | DISTINCT/UNION 保持 NULL、重复语义及首次出现顺序；输出拒绝后清理 |
| 真进程崩溃恢复 | 32 轮 `_Exit(87)`，每轮物理只读前后文件 SHA256 相同，可写恢复后图值为 7、额度可复用、完整性检查通过 |

`required-cases.json` 将上述条件映射到 19 个必须实际执行且成功的用例。最终源码的 12 个受影响模块共 170 项测试通过，无禁用、跳过或失败；原始 XML 和日志随本记录保存。额外复用相同源码/worker 的 1,275 个隔离案例，覆盖 19 条分配故障路径，见相邻目录 `20261016-owned-catalog-keys-and-schema-encoding/zyx-owned-catalog-keys-all-faults.json`。长路径抽样最多 96 个点，短路径穷举；不宣称穷举所有故障。

新复验的 32 轮崩溃恢复始终占用 1,572,864 字节。该观察证明此固定恢复序列复用了空间，不替代 O6 持续吞吐/P95/积压测量，也不代替 S6.3 长期资源测试。未启动新的性能评测。

## 复现与限制

在现有 Linux Clang19 coverage Ninja 构建环境中，先 `source /workspace/zyx-cloud-env.sh`，再运行本目录 `run_selected_tests.py`。脚本只构建列出的 12 个测试源文件和 core，并单独链接，不构建或运行全量测试目标。崩溃复验命令为：

```bash
python3 scripts/test_transaction_resource_faults.py --worker /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-catalog-keys-worker --output /tmp/zyx-s54-crash-recovery.json --crash-recovery --crash-cycles 32 --progress-every 8
```

`proof.json` 记录 worker、证据及源码清单 hash。平台实测仅 Linux x86_64/Clang19/libstdc++14；Windows 错误注入分支存在于源码，但不冒充本轮 Windows/macOS 运行结果。没有提高资源限额，没有修改测试断言，没有全量测试或新性能结论。保留证据和必要 worker；临时数据库由隔离运行器清理。
