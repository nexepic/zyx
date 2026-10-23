from pathlib import Path
for lang in ['zh','en']:
 b=Path('docs/apps/docs/content/docs')/lang/'zyx/architecture'
 p=b/'optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('冷选读在现有批窗口复用同一解码块，不能另建缓存。', '单字段类型化冷选读已在既有 64-owner 窗口按请求顺序复用同引用连续段的解码块，见 `20261022-core-cost-cold-selected-window/`；多字段／精确计数路径仍待同窗口收敛，不能另建缓存。',1)
 else:
  s=s.replace('Reuse one decoded cold block within the existing batch window, without another cache.', 'Single-field typed cold selection now shares one decoded body per consecutive same-reference run in the existing 64-owner window (`20261022-core-cost-cold-selected-window/`); multiple-field/exact-count paths still need the same-window consolidation, without another cache.',1)
 p.write_text(s)
 p=b/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('冷字段批处理仍须在同一既有窗口内避免按 owner 重复解码，不新增第二份 ROW 缓存。', '单字段类型化冷选读已在同一既有窗口按同引用连续段复用解码体；多字段／精确计数路径仍待收敛，不新增第二份 ROW 缓存。',1)
  s+='\n\n单字段类型化冷选读实现见 `20261022-core-cost-cold-selected-window/`：严格有序、同组且至多 64-owner 的请求在输出前批量核对实际 Entity Map／无竞争 ROW，并拒绝旧显式 selector。若 token 全局归热列所有，立即交回热路径，缺失不读冷块。冷行按请求顺序的同 SlotReference 连续段共用一个解码体；跨无冷行的空隙仍可复用，切换到不同引用时释放原体，不改变输出 ordinal／顺序，不持有多个冷块或新增上下文缓存。借用的文本／scalar 同步消费，复杂值仍走原 owning codec；缺失单元只对有根 owner 输出，NULL 保持独立。原 ROW 交接函数每次实际块解码调用。允许首次输出之前的额度不足退回既有点路径；输出开始后额度／sink／取消异常直接传播，不重放。64-owner 夹具的两个共享冷根只解码 1 次；分块 RAW Edge、NULL／缺失、真实 LargeValue、竞争 ROW、输出后额度异常及退款均有模块测试。多字段／精确计数和公开转换后的真实查询验收仍未完成，没有性能时延结论，进度仍 90%。\n'
 else:
  s=s.replace('Cold batches still need to avoid repeated per-owner decoding within the same existing window, without a second ROW cache.', 'Single-field typed cold batches now share a body per consecutive same-reference run in the existing window; multiple-field/exact-count consolidation remains pending, without a second ROW cache.')
  s+='\n\nSingle-field typed cold selection is implemented in `20261022-core-cost-cold-selected-window/`. Strictly ordered same-group requests of at most64owners batch-authenticate actual Entity Map/no competing ROW and reject old explicit selectors before output. A globally hot token immediately returns to hot routing; hot absence never reads cold blocks. Consecutive same-SlotReference runs in request order share one decoded body, including gaps with no cold row. Switching references releases the original body, preserving output ordinal/order and retaining neither multiple bodies nor a new context cache. Text/scalar bytes are borrowed synchronously; complex values retain the original owning codec. Missing cells are emitted only for owners with roots, independently of NULL. The original ROW handoff runs at each actual block decode. Admission failure before output may decline to the existing point path; admission/sink/cancellation failures after output propagate without replay. The64-owner fixture decodes two shared cold roots once; module tests cover split RAW Edge, NULL/missing, real LargeValue, competing ROW, quota failure after output and refunds. Multiple-field/exact-count consolidation and real queries after public conversion remain pending; this makes no latency claim and progress stays90%.\n'
 p.write_text(s)
