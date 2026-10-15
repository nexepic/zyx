# CASE, map projection and IN ownership

CASE 分支和映射投影数组在 backing 分配前申请额度；映射投影项支持深拷贝，扩容复制失败保持原键/子表达式。IN 列表值接入嵌套复制额度，静态解析通过已计费列表构建器消除未计费临时数组。1003 项相关测试、最终 2 项深拷贝拒绝/跨账户测试和 105 个独立案例通过；表达式探针扩展至 12 种形状且漏计为零。

Component evidence only:22/34 (64.7%). Write items, other workspaces and steady-state acceptance remain open. Linux/clang19/libstdc++14; no Windows/macOS runtime, RSS or Kuzu benefit claimed.
