### 2026-10-20 R3.2：完整大型 ROW carrier 的双向发布与精确回收

证据：`benchmarks/architecture-design/decisions/20261020-r32-large-row-inverse/`。

受控逆转换现可复用完全匹配的既有完整 ROW carrier，也能在字段变化后创建新 carrier。新大值 ID、数据/manifest、内容索引 COW 和 ROW 引用随 native 根/页退休同一次 A/B 发布；同批多个 owner 相同内容只分配一次身份。前向及逆向复用同一个元数据私有内容索引准备器；既有树只修改变化路径，根与子页分别核对物理代际。

完整 owner-neutral version-3 编码新增同账户 PMR 输出，不复制成标准整包缓冲；与原编码逐字节一致。目录预检先验证有序 token，额度拒绝及编码中取消均退款。B+Tree 覆盖层的内部 separator、条目和分裂/合并暂存改用现有精确计费容器，长内容键解压后的容量不再只靠固定短键暂存信用。

定向覆盖已有/新建 carrier、Node/Edge、同批跨owner去重、字段改后逆转换、只读重开/旧pin、A/B完整恢复、128 KiB额度拒绝与私有准备后取消重试；返回ROW后继续普通大值写入，既有精确GC保留两位owner共享的当前carrier，回收3个无引用身份，并保证旧native pin可读。native描述符仍活跃时的通用大值SET/引用删除/GC保持保护。旧拒绝测试改为验证已实现的成功路径，并用恢复后不同载荷、故障和额度测试保留原子性检查；直接include、提取回调的metadata参数、fixture owner策略及gtest同一行标签问题均已修正，失败日志保留。

最终仅23个相关存储模块回归，确切计数见 proof.json。未运行全量单测或性能窗口。完整schema/统计/标签协调、native活跃期间大值引用生命周期、多组、通用维护、完整原生成本/同执行器ROW对照和生产/R7准入仍待完成。**无新增阶段验收，13/24（54.2%），不提交、不推送。**
