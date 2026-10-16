# S6.3：长期资源、恢复和清理验收

结论：S6.3 已验收。验收对象是重复运行后的资源归还、空间复用、长快照压力恢复及崩溃清理，不是 S5.3 全部预算接入或 S5.5 稳态性能。S6.1 最终源码冻结和 S6.2 性能对照仍独立待办。

## 预定工作量与结果

| 检查 | 结果 |
|---|---|
| 隔离循环 | 256 轮，每轮严格执行 6 项合同，共 1,536 次，全部通过 |
| 原生重开与结果逃逸 | 16,384 次；每次校验 QUERY/CACHE 归还、结果仍有效，最后所有者销毁后域释放 |
| 并发读者/写者/维护 | 16,384 次写迭代；分配段数不超过 2，旧读者有效，维护完成后保留代积压为 0，重开验证内容 |
| 长快照与磁盘压力 | 每轮触及固定文件上限，失败不发布；释放快照并回收后可再次写入，始终不突破上限 |
| scratch 压力与清理重试 | 每轮校验图 pin 不阻止私有空间复用、满 CACHE 下受保护 MAINTENANCE 清理、准入拒绝后原句柄可重试 |
| 真进程崩溃 | 同一数据库 512 轮 crash/物理只读/可写恢复；每轮保留用户值 7，重新使用额度，完整性检查通过 |
| 恢复文件空间 | 512 轮始终为 1,572,864 字节，无逐轮增长 |
| 隔离循环总时长 | 1,187.93 秒，约 19.8 分钟；按预定轮数结束，不重试到通过 |
| 同进程补充 | 回调修改后单进程打乱顺序执行 16 轮，96 次合同、1,024 次重开和并发写迭代全部通过 |
| 同进程峰值 RSS | 30,584,832 字节（约 29.2 MiB）；独立报告，不当作预算计数或绝对 RSS 上限 |

隔离轮次与崩溃轮次在运行前固定为 256/512。每个用例执行原有断言，不改变额度。原始日志被当前验收器再次核对，要求每个必需用例在每轮都运行且成功，不能只相信最终 XML；另有 10 项验收器单元测试，拒绝缺失、跳过、失败和重复/替换用例。长分配故障路径的抽样合同仍见 S5.4，不因本次长期循环改称穷举。

## 源码身份、复现与限制

隔离 worker/测试二进制来自 `05cba31b`，回调变更后的同进程二进制来自 `e9d18095`（其 170 项资源模块测试已通过）。之后 `1714cd1e` 仅修改最短路径私有工作区，相关 16 项过程测试通过；本次 6 项循环合同不调用该过程，不重复无关的长期运行。各二进制 SHA256、来源和原始证据摘要记录于 proof.json；源码最终冻结仍由 S6.1 负责。

```bash
source /workspace/zyx-cloud-env.sh
python3 scripts/test_resource_recovery_soak.py --test-binary /workspace/zyx-optimization-artifacts/frozen-workers/zyx-resource-acceptance-tests --worker /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-catalog-keys-worker --output /tmp/zyx-s63-resource-soak.json
python3 scripts/test_resource_recovery_process_soak.py --test-binary /workspace/zyx-optimization-artifacts/frozen-workers/zyx-owned-callback-resource-tests --output /tmp/zyx-s63-streamed-hash-rss-soak.json
python3 scripts/test_resource_recovery_soak_validation.py
```

RSS 初次观察受测量器整文件读入大二进制干扰，已弃用；最终复验使用分块 SHA256，新的监督进程仅有一个测试子进程。RSS 含运行库、映射和插桩开销，与可计费分配分开。运行平台仅 Linux x86_64/Clang19/libstdc++14，不声称多日 soak 或 Windows/macOS 运行结果。没有全量单元测试、新性能比较或 Kuzu 胜负结论，也未以本项替代 O6 吞吐/P95 验收。原始日志和 JSON/XML 在 raw-evidence.tar.gz 中，隔离数据库已自动清理。
