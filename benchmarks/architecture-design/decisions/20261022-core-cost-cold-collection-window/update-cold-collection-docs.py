from pathlib import Path
for lang in ['zh','en']:
 b=Path('docs/apps/docs/content/docs')/lang/'zyx/architecture'
 p=b/'optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('借用只覆盖同步作用域，不能另建缓存。', '完整属性集合的计数／准入／值构造也已复用该同步窗口，见 `20261022-core-cost-cold-collection-window/`。借用只覆盖同步作用域，不能另建缓存。',1)
 else:
  s=s.replace('Borrowing is synchronous only and adds no cache.', 'Complete collection counting/admission/value construction also shares that synchronous window (`20261022-core-cost-cold-collection-window/`). Borrowing is synchronous only and adds no cache.',1)
 p.write_text(s)
 p=b/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('此处完成选读窗口复用，不代表完整值 collection API、公开转换后的真实查询／生命周期或最终性能门禁完成；进度仍 90%。','此处完成选读窗口复用；完整值 collection API 的后续证据独立记录，不能替代公开转换后的真实查询／生命周期或最终性能门禁，进度仍 90%。',1)
  s+='\n\n完整属性集合窗口见 `20261022-core-cost-cold-collection-window/`：同一 owner 的实际目录／计数、逐字段预准入、最终 owning 值构造均在同一同步冷块借用内执行；原入口、排序／NULL／复杂类型、原 LargeValue 内容认证及预算／slots admission 合同不变。原结果在借用退出后才返回，不能带出冷解码视图；构造失败不能以额度回退重放准入。未进入值构造的窗口准入不足可以回到原有点读路径。真实 LargeValue 夹具同时读取热整数、冷长 ID 和 8193 字节文本，并证明整段仅一次块解码、恰好三次逐字段准入，异常／原生命周期相关模块继续验证。至此点查／选读／精确计数／完整集合共用同一解码基础；公开转换、变更及实际回收／旧 pin／退役、成熟全库成本和最终性能门禁仍待完成，进度仍 90%。\n'
 else:
  s=s.replace('This completes selected-window sharing, not the full collection API, real public-conversion query/lifecycle acceptance or final performance gates; progress stays90%.', 'This completes selected-window sharing; subsequent complete-collection evidence is recorded separately and cannot replace real public-conversion query/lifecycle acceptance or final performance gates. Progress stays90%.',1)
  s+='\n\nComplete-collection windows are in `20261022-core-cost-cold-collection-window/`. Actual owner directory/count, per-field pre-admission and final owning-value construction execute inside the same synchronous cold-block lease. Original entry points, sorting/NULL/complex types, actual LargeValue content authentication and budget/slots-admission contracts remain. The owning result returns only after the borrow exits and cannot expose a decoded view; construction failure cannot trigger quota fallback/replayed admission. Window admission failure before value construction may retain the original point path. The real-LargeValue fixture reads a hot integer, cold long ID and8193-byte text with one block decode and exactly three per-field admissions; related exception/lifecycle modules remain covered. Point/selected/exact-count/complete-collection reads now share the same decoding foundation. Public conversion/mutation and actual reclamation/old-pin/retirement, mature whole allocation and final performance gates remain pending; progress stays90%.\n'
 p.write_text(s)
