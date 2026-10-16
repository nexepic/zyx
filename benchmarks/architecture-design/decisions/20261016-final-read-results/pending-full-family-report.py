from pathlib import Path
import json,re
r=Path('/workspace/zyx');out=r/'benchmarks/architecture-design/decisions/20261016-s03-complete-versioned-baseline'
d=json.loads((out/'family-outcomes.json').read_text());reads=json.loads((out/'read-comparison.json').read_text());reg=json.loads((out/'registration.json').read_text())
def outcomes(row,engine):return ', '.join(f'{k}={v}' for k,v in row['outcomes'][engine].items()) or 'N/A'
def number(value):return f'{value:.3f}' if value is not None else 'N/A'
for lang in ['en','zh']:
 if lang=='zh':
  text='''# S0.3：同条件、分版本基线与完整场景结果

本交付完成基线资料及已登记完整场景的执行结果记录；**不等于所有场景成功、不等于 S6.2 综合性能通过，也不证明超过 Kuzu**。Native 的“current”标签严格指冻结源 `6a735df8`；生产修复源 `03436b7a` 只有独立的正确性复验及取消的 O6 预热记录，不能把旧源数字标成最新修复版。

## 来源、资源与计时范围

原登记、冻结 worker/harness hash、准备失败和续跑协议都随本目录保存。控制版本保持不可变；只恢复因外部暂停而中断的聚合案例，不重跑已成功的读样本。实际格式与每案例 worker/runner hash 在压缩证据中。复制准备输入的 prefix/suffix 来源如实保留，未声称从空库完成最终版本导入，导入耗时也不混入读延迟。

同机单 CPU affinity、单查询线程、6 GiB 地址空间与 2 GiB 单文件上限；Kuzu 64 MiB buffer pool / 2 GiB mapping，Native QUERY 64 MiB、总域和类别上限按冻结实现记录，内部私有写工作区变化在先前修复证据披露。该环境不是 6 GiB RSS/cgroup 等价实验；实际主机 cgroup、CPU、工具链和依赖见 registration。Native 默认语句时限为 30 秒，Kuzu 合同 helper 使用剩余家族时限（至多 30 分钟）；该差异明确保留，不把超时/失败差异当成同截止时间的性能胜负。默认同步及确认协议不降低。缓存为热态，不宣称清空 OS cache 或冷态结果。

正式读部分：20 个可比较工作负载，24 个独立 profile 进程、120 个进程×工作负载分布，1,200 个有效样本；每查询每进程 3 次预热、10 次拥有输出测量。不能把 120 个分布冒称 120 个独立进程。P50 汇总为独立进程 P50 的中位数；保留进程区间，10 样本 P95 只作描述，不通过尾延迟门禁。

其余完整合同场景按预先登记的独立家族策略执行，逐值/类型/NULL/缺失/顺序/重复核对，保留原子性及故障结果。除维护的预热/测量窗口外，它们是完整规模的语义与操作成本观察，不是重复热态查询延迟或保护场景通过证明；不补造 3 次预热/10 次重复数据。Native IPC 计时含序列化、解析、校验与 trace 保留，Kuzu Python cursor 定时含 execute/drain/close 和校验、在 trace 保留之前结束；这些观察开销并不完全相同；C++ 读入口和 Kuzu Cypher 的绑定/计划成本不同，不能把这些不同层的 timer 相加或混用。

## 完整合同覆盖与失败

117 个登记槽位覆盖 13 家族、三方、三轮；实际执行进程数与未执行/N/A 分开。只把完整 oracle 成功的案例写作 completed。不可测的原版准备、确定性失败和时限结果保留为 N/A/不确定，不填零，不据此推导提速。后续同源同场景的失败轮次依预登记策略引用原失败，而不冒称独立执行。

| 家族 | control | 冻结 current6a | Kuzu |
|---|---|---|---|
'''
 else:
  text='''# S0.3: same-envelope versioned baseline and complete family outcomes

This delivers the baseline records and outcomes of the registered full scenarios. **It is not an all-scenarios pass, S6.2 overall performance acceptance or Kuzu superiority.** Native `current` means frozen source `6a735df8`. Repaired production source `03436b7a` has separate correctness evidence and canceled O6 warmups; original-source numbers are never labeled newest-source performance.

## Provenance, resources and timing scopes

Original registration, immutable worker/harness hashes, preparation failures and continuation policy are retained. Only an externally interrupted aggregate case restarts; successful read samples are not rerun. Per-case actual formats and worker/runner hashes are in the compressed evidence. Prepared input prefix/suffix lineage stays explicit: no fresh final-version import or import-time claim is inferred from continued fixtures.

Same host, one CPU affinity, one query thread, 6 GiB address-space and 2 GiB per-file limits; Kuzu uses a 64 MiB pool / 2 GiB mapping, Native QUERY is 64 MiB with frozen domain/class limits. Internal private-write-grant changes remain disclosed by prior repair evidence. This is not a 6 GiB RSS/cgroup-equivalence experiment; actual host cgroup, CPU, compiler and dependencies remain registered. Native statements retain their default 30-second deadline, while the Kuzu contract helper uses the remaining family budget (up to 30 minutes). This difference is explicit: timeout/failure outcomes do not establish equal-deadline performance superiority. Default sync/acknowledgement stays enabled. Warm OS cache is not cold-cache evidence.

Formal reads contain 20 comparable workloads, 24 independent profile processes, 120 process-workload distributions and 1,200 valid owning-output samples. Each query/process has three warmups and ten samples. Do not call the 120 distributions independent processes. Aggregate the median of independent process P50s and retain process ranges; ten-sample P95 is descriptive, not a tail-protection pass.

Other full contract cases follow the preregistered isolated-family policy with complete values/types/NULL/missing/order/multiplicity checks and atomic/failure observations. Except maintenance windows, these are full-scale semantic/operational observations, not repeated warm-query distributions or protection passes; no missing warmups/repetitions are invented. Native IPC includes serialization/decoding/validation and trace retention; Kuzu Python cursor timing includes execute/drain/validation/close and ends before trace retention. Observer overheads are therefore not identical. Native C++ read and Kuzu Cypher entry costs differ; timers from these scopes are not summed or pooled.

## Complete contract coverage and failures

117 registered slots cover 13 families, three engines and three rounds; executed processes are distinguished from skipped/unsupported slots. Only complete oracle success is `completed`. Failed original preparation, deterministic failures and cutoffs remain N/A/inconclusive, never zero latency or inferred speedup. Later unchanged failed-family rounds reference the original failure under the preregistered rule, not fictitious independent execution.

| Family | control | frozen current6a | Kuzu |
|---|---|---|---|
'''
 for row in d['families']:text+='| '+row['family']+' | '+' | '.join(outcomes(row,e) for e in ['control','current','kuzu'])+' |\n'
 text+=('\n实际执行独立合同进程：' if lang=='zh' else '\nExecuted independent contract processes: ')+str(d['executed_independent_processes'])+'. '+('各状态计数：' if lang=='zh' else 'Outcome counts: ')+json.dumps(d['status_counts'],ensure_ascii=False)+'.\n'
 if lang=='zh':text+='''
100,000 组 Native 拥有输出达到固定额度时，完整查询仍记 N/A；分区逐值核对不能改写成该查询成功。Kuzu 二级标量索引不支持的真实能力记录保留；原错误的无限 mapping probe 已在零有效样本前修正，失败与唯一重试记录均保留。动态字段缺失语义和 Native fence/回收接口不伪造 Kuzu 等价实现。

原 source6a 维护的 Native 隐式完整删除结果与 Kuzu 无返回语义不同，且 Native 在可选缓存处失败；两者不构成可比较的稳态延迟。修复版对齐显式标量返回的 O6 窗口在两个预热后因本轮范围收敛主动取消，没有测量样本，没有稳态通过。多客户端保守 fence 与 Kuzu 事务行为差异单独记录。维护全局积压、旧案例的 Native peak RSS、设备物理 I/O 等未提供的量不填 0；旧 elapsed 没有 rss_scope 时仅能解释为 driver RSS。

## 正式读比较

以下均为进程 P50 中位数（ms）；倍数为 Native/Kuzu，越大表示 Native 越慢。完整进程 P50 区间、P95、样本和能力/失败状态见 [read-comparison.json](read-comparison.json)。原控制两种输入准备均失败，因此所有 control 提速列保持空，不能声称主要目标降低 10% 或保护门禁通过。

| 规模 | 工作负载 | Native ms | Kuzu ms | Native/Kuzu |
|---|---|---:|---:|---:|
'''
 else:text+='''
Native 100,000-group owning output remains N/A at the fixed admission cap; partitioned value checks do not make the rejected full query succeed. Kuzu unsupported secondary scalar indexes retain the real capability witness. Its erroneous unlimited-mapping probe was corrected before valid samples for the affected case; failures and the single targeted retry remain. Dynamic missing-key semantics and Native fence/reclaimer APIs have no fabricated Kuzu implementation.

Original source6a maintenance has unequal implicit full-result Native versus no-return Kuzu semantics and a Native optional-cache failure; it yields no comparable steady-state latency. Repaired-source scalar-output O6 was externally canceled after two warmups for the five-delivery scope, with no accepted measurement or steady-state pass. Multi-client conservative-fence versus Kuzu transaction differences stay explicit. Unavailable global backlog, older Native engine-peak RSS and physical-device I/O are not zero. Older elapsed files lacking rss_scope are driver RSS only.

## Formal read comparison

Median independent-process P50s in ms; Native/Kuzu above one means Native is slower. Full process ranges, P95s, samples and capability/failure outcomes are in [read-comparison.json](read-comparison.json). Both control input preparations failed, so all control-improvement fields stay null: no 10% primary improvement or protection pass is inferred.

| Scale | Workload | Native ms | Kuzu ms | Native/Kuzu |
|---|---|---:|---:|---:|
'''
 for row in reads['cases']:
  text+='| '+row['scale']+' | '+row['workload']+' | '+number(row['engines']['current'].get('median_process_p50_ms'))+' | '+number(row['engines']['kuzu'].get('median_process_p50_ms'))+' | '+number(row['current_over_kuzu_p50'])+' |\n'
 if lang=='zh':text+='''
Kuzu 目标尚未达到。S2.5、S3.5、S4.4、S5.5、S6.2 的生产/资源/端到端性能准入保持独立，不能以本基线资料交付替代。最终修复源尚未完成同版本全矩阵比较。CPU 时钟偏差与有界堆窗口见 [S0.4](../20261016-s04-hotspot-and-falsification/proof.json)；计数累计字节不等于 RSS 峰值或设备 I/O。

证据：[全家族状态](family-outcomes.json)、[压缩证据与逐文件 hash](full-family-files.json)、[正式读原始资料](../20261016-final-read-results/proof.json)、[修复源正确性](../20261016-cache-decline-delete-budget/proof.json)、[可选决策/文档](../20261016-s64-optional-decisions-and-docs/proof.json)。归档解压后的每个文件 hash 均已逐项验证。源码、输入和结果版本保持分离；本交付不新增算法、不调整预算或持久化承诺。
'''
 else:text+='''
Kuzu superiority is not achieved. S2.5, S3.5, S4.4, S5.5 and S6.2 production/resource/end-to-end admission remain independent. This baseline-data delivery does not replace them or establish a same-version newest-source whole-matrix comparison. CPU clock disagreement and bounded heap windows remain in [S0.4](../20261016-s04-hotspot-and-falsification/proof.json); cumulative bytes are neither RSS peaks nor physical device I/O.

Evidence: [family outcomes](family-outcomes.json), [compressed evidence/file hashes](full-family-files.json), [formal read raw evidence](../20261016-final-read-results/proof.json), [repaired-source correctness](../20261016-cache-decline-delete-budget/proof.json), and [optional decisions/documentation](../20261016-s64-optional-decisions-and-docs/proof.json). Every archived file was verified after decompression. Source/input/result versions remain separate. This delivery adds no algorithm and changes no budget or durability promise.
'''
 p=out/f'{lang}.md';p.write_text(text)
 for target in re.findall(r'\]\(([^)]+)\)',text):assert (p.parent/target).resolve().is_file(),target
print('Versioned baseline reports generated; acceptance/ledger remain unchanged pending review.')
