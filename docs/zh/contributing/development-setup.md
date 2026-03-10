# 开发环境设置

## 前置要求

- C++20 编译器
- Meson, Conan, Ninja
- Google Test
- ANTLR4（用于解析器更改）

## 构建

```bash
conan install . --output-folder=buildDir --build=missing
meson setup buildDir
meson compile -C buildDir
```

## 测试

```bash
./scripts/run_tests.sh
```
