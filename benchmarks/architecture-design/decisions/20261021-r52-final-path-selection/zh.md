# R5.2 最终实现选择：canonical key-only 与批量主记录复用

本轮候选冻结为现有canonical全局双向key-only B+Tree（index0x8000000000000002，BLVL_EMPTY）加已实现的有界主记录／路由复用。类型成员候选／扫描由既有占用成本选择；label/type同pin统计、引用行宽和语义保护后的JOIN／扩展顺序使用R5.3实现。不引入新的可信拓扑持久协议。该选择仍须R7图读写／空间／资源准入，尚不计阶段完成。

覆盖树原型的两方向、普通写协调、旧pin、恢复／退休、overlay和同执行器已验证；但每个候选仍与主Edge认证，物理只读对照setup还须显式全局核对。它没有消除canonical主记录工作，且完整90边热点实配从1703936增加至1835008字节（+7.69%），原始记录在20261021-r5-lifecycle-protocol。不能扣掉仍须维护的canonical树，也不能删除主记录检查来制造收益。没有证明能够替换完整性和旧writer协议的持久生产开关，因此拒绝自动生产采用；实验入口／显式writer选项保持隔离。这里没有宣布覆盖路径性能失败，尚未测量新的正式时延。

共享分段只作本轮架构成本对照，不恢复per-node root/delta：相邻批需要EdgeId／NeighborId／TypeId、方向／可见性及continuation和目录；当前合同仍须主记录检查，额外目录／COW／退休与更新成本尚无可抵消的收益证明。原始logical body的17-byte canonical key、33-byte covered key/value及24-byte packed segment item不能当成完整实配成本；分段没有持久完整生命周期，不是可准入的第三生产候选。不扩展一个缺少收益依据的新存储子系统。

canonical选择已经持久于现有IndexRoot／格式合同，兼容的旧ROW writer仍维护同一主记录和双向树；新普通索引图像不改wire格式。无需新增一个不能由旧writer识别的配置bit。保持当前主Edge认证，所有获准路径均使用已验证的原子发布／资源边界。R4完整规模只读图索引核验、R5.3原始／优化IR六项各两套完整规模对照均通过；R7时延／稳态保护仍待执行，不以当前选择提前接受R5.2。
