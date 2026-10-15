# Write-item and expression-map ownership

SET/MERGE/REMOVE 的动作项字符串和 backing 接入预算。CREATE/MERGE 的表达式映射复制先申请桶/节点/键额度，再在同一所有者下克隆子表达式；中途取消释放部分副本，源保持完整。1006 项相关测试、补充 REMOVE 后 714 项定向测试及 105 个独立案例通过，完整计划探针扩展至 10 种形状，表达式 12 种形状，漏计为零。编译/运行工作区和稳态合同继续治理。

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no Windows/macOS runtime, RSS or Kuzu benefit claimed.
