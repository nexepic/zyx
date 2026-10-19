# R3.2：五个有界 payload 与准入实现步骤

父提交：`6fa1d4e5a2c39f678b8cf5e10ee493616069800b`，先登记后实现。五个步骤属于**同一尚未完成的 R3.2**，不是五项已验收的 R0–R7 交付。总进度仍从活动优化文档派生：13/24；R3.2 为进行中。

| 步骤 | 已实现合同 | 验证 |
|---|---|---|
| 列记录 | 精确 64 字节头；int/double 共 576 字节、bool 共 72 字节；64 槽定位、身份/代际、presence/live/NULL 子集、缺失/NULL payload 归零；至少 48 个 present 槽 | 精确偏移/小端、47/48/64 边界、missing/NULL、int/double 身份、负零/NaN/无穷原始位、错误类型/owner、全部截断长度 |
| 组头 | 64 字节；owner/group/generation、hybrid 模式、实际成员和列计数、保留位归零 | 错误身份、空组、稀疏列、计数上限、保留字节 |
| 描述符 | 192 字节；数据库/语义代际；八个独立 PageId+generation 根、flags/保留位归零 | 精确偏移、空根/半空根、未来根/描述符代际、错误数据库 |
| 成员与定位 | 16 字节 owner/token/source；16 字节 locator；28 字节列目录和 24 字节组目录记录 | 唯一来源标签、owner/token/group 身份、page/slot/generation 比较、保留字节及截断 |
| 转换门禁 | 九类原生成本逐项 known 证据，ROW 下界/候选上界；至少 10% 节省、完整峰值不超过文件上限，最多 64 owner/4096 成员/1MiB 暂存且不超剩余账户 | 未知成本/峰值拒绝、精确 10% 和取整边界、溢出、工作量/资源上限、不一致峰值拒绝、完整预留页池反例 |

代码：`benchmarks/architecture-design/property-subblock/PropertySubblockCandidate.hpp`。固定大小 payload 操作不分配堆；列编码使用调用方已准入的精确长度目的缓冲，其他编码返回固定数组。这**尚未实现**转换账户预留、查询路由或原生完整成本估计。known 标志须由后续经过审计的原生估计器提供，门禁不能自行推断。九类覆盖数据/映射预留页池、描述符、catalog/allocator、页目录/路由、大值 manifest/content、查询索引、退役队列、旧 pin；未知或未证明可抵消的成本不能标已知。峰值包含当前分配的旧/新/退役/pin 容量，至少不小于两个稳态总量。测试是合成门禁输入，不是原生分配实测。

没有连接文件写入或原生调用者。payload 标签/版本仅属实验，不是已分配的 MetadataPageKind 或 Superblock 格式更新；原生 Superblock 仍为 31，格式 profile 为 5。调用方提供的期望身份不能替代外层原生 page/slot 认证。校验和、selector/extension/column 双向完整性、实际 B+Tree 排序和 framing、原生 slot 可用上限与分配/代际生命周期仍须接入。NaN 原始位保真不能替代执行器比较/聚合语义。

不压缩逻辑 ID。group/slot 运算拒绝 ID 零，以及 uint64 最后一个组中不存在的第 64 槽。标签不参与放置。codec 不自动提升/降级、不改写 owner、不分配页池、不持久化描述符根。

## 检查与复现

最终 20 项定向用例通过，包含 1 万次确定性字节变异：可接受变异重新编码保持精确字节，非法结构拒绝。标量 payload 变异可能结构合法，仍必须依赖外层原生认证。`first-tests.json` 为早期 18 项通过，`final-tests.json` 为 20 项通过。CMake/CTest 复现同组用例通过。初次编译发现同一行两个 EXPECT_THROW 造成 GTest label 冲突，分行修正后才有首次可运行测试；未因此改变生产 API。

AddressSanitizer 和 UndefinedBehaviorSanitizer 在 `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` 下 20 项通过。首次 sanitizer 进程虽通过 GTest，但因环境 ptrace 限制下 LeakSanitizer 无法工作，进程退出 1；保留 `sanitized-tests.json` 和 `sanitizer-process-status.json`，不能说该进程成功。最终见 `address-undefined-tests.json`，不宣称泄漏检测通过。实际仅运行 Linux x86_64/Clang 19.1.7/GTest 1.15.0；可移植 C++20/固定小端编码/CMake 不代表 Windows/macOS 已测。

安装 CMake/GTest 后复现：

```bash
python3 benchmarks/architecture-design/decisions/20261019-r3-codec-five/run-tests.py /tmp/zyx-r3-codec-build -DCMAKE_BUILD_TYPE=Debug
```

非默认 GTest 安装可传 `-DCMAKE_PREFIX_PATH=...`；`-DZYX_CANDIDATE_SANITIZERS=ON` 启用可选 Clang/GNU sanitizer 构建。主工程既有 relocatable property 测试 glob 同样发现新用例。未跑全量单测或正式性能窗口。

## 剩余验收边界

R3.2 仍需原生格式/root/fence/kind/读写/完整性/搬迁/退役原子接入、共享映射及独立认证 extension/大值、计费的有界双向转换和单字段降级、图/索引/overlay 一致性、旧新 pin/重开/复用/退役/配额/fence 故障、完整原生实际分配与估计对比，以及同一 R1 执行器下 ROW 对照。继续遵循 R3.1 全矩阵，不因五个步骤完成而缩减。R3.3 生产和 R7 性能门禁仍未准入，不宣称提速、转换已获准或空间已节省。
