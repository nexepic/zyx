# 原生评测进程的独立峰值 RSS

旧读取窗口的 `RUSAGE_CHILDREN` 包含 Git/启动器等辅助进程，而且 POSIX 的 rusage 还可能保留 exec 前父进程峰值；不能把它当作原生引擎独立峰值。原记录保留，不据此补记资源验收。

评测 CLI 可通过 `ZYX_COMPARE_PROCESS_MEMORY_REPORT` 输出独立 JSON 文件，在全部查询计时结束后采集。Linux 使用当前 exec 后内存映射的 VmHWM，Windows 使用 PeakWorkingSetSize，其他 POSIX 的 rusage 明确标记可能包含 exec 前峰值。默认 JSONL 协议不变；现有输出路径拒绝覆盖；Windows 增加标准 psapi 链接。当前及冻结旧核心使用相同评测入口源码。

4 个真实小规模 CLI 接入检查通过：当前、原对照、未启用报告、拒绝覆盖。刻意驻留约282MB的 Python 父进程下，旧 children rusage 约300MB，而两个原生进程报告约17.8MB/12.3MB，证明采集未把父进程占用算入。数字仅用于采集验证，不代表正式资源比较或性能胜负。范围仍包括原生 CLI 自身的准备及结果输出，不等于仅查询工作区。Linux 之外的分支未在本环境执行。

首次验证器漏读 stderr 错误事件，原日志保留；改为检查两条 JSONL 流后，在新目录完成全部检查，没有修改错误断言或 C++行为。进度31/34（91.2%）不变。
