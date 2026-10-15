# Output-variable and result-column ownership

36 个原生算子增加可计费输出变量接口，保留旧公开接口。只读结果列名先在 QUERY 下生成，再预付实际标准数组复制上界，最后将已有额度转交给结果保留账户。Windows 调试空容器代理在结果构造前准入；源审计不替代 Windows 运行。公开结果的左值列数组也先申请额度再复制，自赋值无操作。1063 项相关测试和 106 个独立案例通过；最终空代理调整后 57 项测试和 10 个独立案例通过。列复制探针拒绝时大载荷分配 0 次，成功 1 次。其他 mutation/procedure 输出和运行/编译工作区、聚合无别名的渲染回退及稳态验收仍待闭环。

Component evidence only:22/34 (64.7%). Linux/clang19/libstdc++14; no macOS/Windows runtime, RSS, new performance campaign or Kuzu benefit claimed.
