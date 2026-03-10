# 测试指南

## 单元测试

位于 `tests/src/`，与源代码结构匹配。

## 集成测试

位于 `tests/integration/`，用于端到端工作流。

## 运行测试

```bash
# 所有测试
./scripts/run_tests.sh

# 特定测试
meson test -C buildDir <test_name>

# 生成覆盖率报告
./scripts/run_tests.sh --html
```
