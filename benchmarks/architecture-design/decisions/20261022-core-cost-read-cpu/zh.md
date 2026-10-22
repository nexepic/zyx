# P0 读取独立 CPU 归因

冻结引擎 archive SHA 与 P0 一致，仅为观察器增加 Linux/x86 的线程 CPU 定时器 PC 采样。原始样本与不采样控制均为同一观察器、同一数据和 12 次只读查询；核验结果逐条保留。只在查询窗口采样，复制/打开不在窗口；记录用户/内核 CPU、wall 和有界信号 overrun。385/371 个独立中断 PC，权重 tick 不是更多独立样本。源码、观察器、引擎 SHA 在 final-observer-freeze.json；按 build recipe 重建前先 git archive 5500a068 的 include/apps/compare_benchmark/lib/inputxx/include 至 /workspace/zyx-p0-profile-source。

不采样控制的过滤/分组 12 次总 wall 为 1.495/1.477 秒，用户 CPU 1.435/1.380 秒，内核 CPU 0.060/0.097 秒，估算 off-CPU 均不足 0.1%。这支持当前热读主要消耗 CPU，不能将差距归咎于磁盘等待。采样进程总 CPU 相对控制增加约 3.2%/1.4%，只用于粗粒度归因，不进入正式延迟。

互斥叶 PC 分桶中，过滤/分组的解码与记录验证约 13.4%/12.8%，校验和 9.7%/12.4%，元数据查找缓存校验 10.0%/7.9%，clock/runtime 8.8%/9.1%，记录对象缓存 5.3%/5.2%。其余类别与函数表完整保留；约 28% 包含未归因/内部 libc，不强行分摊。stripped libc 不采用最近导出符号的伪函数名；只对本机已核对 IFUNC 区间做粗分类。没有调用栈，不能把全部 memcpy/memcmp 归到某个上层调用者，也不能把嵌套 timer 相加。

结论是多个反复执行的定位、解码、对象/身份检查环节共同占用 CPU；真正 filter/group 算术不是单一主要耗时。按既定 P4/P5 减少候选、定位和中间对象，不能只换一棵索引树。gprofng 预尝试存在 timer-loss 警告，已拒绝，见 rejected-gprofng.txt；探索性旧 PC 与不匹配观察器不作为验收数据。没有重跑正式性能矩阵。
