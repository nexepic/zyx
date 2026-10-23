from pathlib import Path
for lang in ['zh','en']:
 b=Path('docs/apps/docs/content/docs')/lang/'zyx/architecture'
 p=b/'optimization.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('以及私有冷块／整根引用准备，均不代表公开转换后的验收；', '以及私有冷块／整根引用准备和单组 Native 不发布规划，均不代表公开转换后的验收；',1)
  s=s.replace('下一步只接入同一 Native 协调器：源 owner／token／ROW fence 认证；新值与去重引用的同批 ID、data／manifest／content-index 及描述符发布；', '下一步只完成同一 Native 协调器的实际发布：保留已接入探测的源 owner／token／ROW fence 认证和同批新值规划，补齐 data／manifest／content-index／描述符的原子安装及完整空间准入；',1)
 else:
  s=s.replace('and private cold-block/whole-root preparation are components,', 'and private cold-block/whole-root preparation plus single-group Native no-publication planning are components,',1)
  s=s.replace('Next integrate only the same Native coordinator: authenticate source owner/token/ROW fences; publish new/deduplicated references with same-batch IDs, data/manifests/content index and descriptor;', 'Next complete actual publication through that same Native coordinator: preserve the probe-integrated source owner/token/ROW fences and same-batch new-value planning; install data/manifests/content index/descriptor atomically with complete space admission;',1)
 p.write_text(s)
 p=b/'native-single-file-storage-plan.mdx';s=p.read_text()
 if lang=='zh':
  s=s.replace('Native 协调器冷 ROW 接入的下一合同：', 'Native 协调器冷 ROW 接入合同（仅不发布探测已实现）：',1)
  s+='\n\n真实 Native 不发布探测证据见 `20261023-core-cost-cold-native-probe/`。既有请求新增默认关闭的 `cold_row_remainder`，仅单组 compact 正向 preparation-only 可用；正式发布、非 compact、多组与逆向形态在源分类前拒绝。源 Entity／ROW／token 仍由原协调器认证，冷准备器、同批整根 proposal／连续 ID、data／manifest 及最终字段／目录／描述符私有图像均走原规划器。字段与整根 proposal 共用原完整 payload 比较与 holder/ID 保留算法；旧字段路径的检查点顺序不变。实际 Node 与 Edge 各 64-owner 的整数＋长 ID 夹具进入私有物理规划，只有一个字段 frame 和一个 group-index frame，不产生冷 per-field 目录；另一夹具从含热字段的旧整根大值提取冷部分，确实计划一个新同批大值及 data／manifest frame。以上均在原 guard 处拒绝发布，文件／I/O epoch／持久根／计数／旧 pin 不变，预算归还。frame 计数不是时延／完整空间节省证明；没有提升原门禁或开放生产，新值 content-index 原子安装、多组及完整冷热变更生命周期仍待完成。编译夹具曾误把返回 owning root 的 readEntity 当成 optional，现按实际接口读取；未修改源语义。进度仍 90%，未全量测试／跑性能／提交／推送。\n'
 else:
  s=s.replace('Next Native coordinator cold-ROW integration contract:', 'Native coordinator cold-ROW integration contract (no-publication probe implemented only):',1)
  s+='\n\nActual Native no-publication probe evidence is in `20261023-core-cost-cold-native-probe/`. Existing requests add default-off cold_row_remainder, restricted to single-group compact forward preparation-only calls. Publication/noncompact/multigroup/inverse shapes reject before source classification. Original coordinator authentication of Entity/ROW/tokens and original planners handle cold preparation, same-batch whole-root proposals/consecutive IDs, data/manifests and final private field/directory/descriptor images. Field/root proposals share the original complete-payload comparison and holder/ID-retention algorithm; old field checkpoint order remains. Actual64-owner Node/Edge integer-plus-long-ID fixtures reach physical preparation with one field frame and one group-index frame and no cold per-field directories. Another fixture extracts cold content from an old whole-root LargeValue containing a hot field, genuinely planning one new batch-local value plus data/manifest frames. All stop at the original publication guard with unchanged file/I/O epoch/persisted roots/counters/old pins and refunded budget. Frame counts prove neither latency nor whole-allocation saving; no gate is raised or production opened. Atomic new-value content-index installation, multigroup and complete hot/cold mutation lifecycle remain pending. An initial compile fixture mistook owning-root readEntity for optional; corrected to its actual API without source-semantic changes. Progress stays90%, with no full suite/timing benchmark/commit/push.\n'
 p.write_text(s)
