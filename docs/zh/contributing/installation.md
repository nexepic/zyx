# 安装指南

## 前置要求

在构建 ZYX 之前，请确保已安装以下依赖项：

### 必需工具

- **C++ 编译器**: 支持 C++20 的编译器
  - GCC 11+ 或 Clang 13+ (Linux)
  - Xcode 14+ (macOS)
  - MSVC 2022+ (Windows)
- **Meson**: 构建系统（版本 0.63+）
- **Conan**: 包管理器（版本 1.60+）
- **Ninja**: 构建后端

### 在 macOS 上安装

```bash
brew install meson conan ninja
```

### 在 Linux 上安装 (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y python3-pip g++ cmake
pip3 install meson conan ninja
```

### 在 Windows 上安装

```powershell
# 使用 Chocolatey
choco install meson conan ninja

# 或使用 pip
pip install meson conan ninja
```

## 从源码构建

### 快速构建

构建和测试 ZYX 的最简单方法：

```bash
# 克隆仓库
git clone https://github.com/yuhong/zyx.git
cd zyx

# 安装依赖、构建并运行测试
./scripts/run_tests.sh
```

此脚本将：
1. 安装所有 Conan 依赖
2. 使用 Meson 配置构建
3. 编译项目
4. 运行所有测试并生成覆盖率报告

### 快速构建（跳过依赖安装）

如果您已经构建过且依赖已安装：

```bash
./scripts/run_tests.sh --quick
```

### 手动构建步骤

如需更多构建过程的控制：

```bash
# 安装 Conan 依赖
conan install . --output-folder=buildDir --build=missing -s build_type=Debug -s compiler.cppstd=20

# 配置 Meson 构建
meson setup buildDir --native-file buildDir/conan_meson_native.ini

# 编译
meson compile -C buildDir
```

## 构建发布版本

用于生产环境构建：

```bash
./scripts/build_release.sh
```

这将在 `buildDir/release/` 中创建优化的发布版本。

## 验证安装

构建完成后，验证 CLI 工具是否可用：

```bash
./buildDir/apps/cli/zyx-cli --help
```

您应该看到 ZYX CLI 的使用信息。

## 安装位置

构建输出位于：

- **CLI 工具**: `buildDir/apps/cli/zyx-cli`
- **库**: `buildDir/zyx_core.a`（内部使用的静态库）
- **测试**: `buildDir/tests/src/*/test_*`

## 可选：系统级安装

要将 ZYX 安装到系统范围（需要 sudo）：

```bash
meson install -C buildDir
```

这将安装：
- 共享库：`/usr/local/lib/libzyx.so`
- 头文件：`/usr/local/include/zyx/`
- pkg-config 文件以便于集成

## 故障排除

### Conan 安装失败

如果 Conan 无法安装依赖：

```bash
# 清除 Conan 缓存
conan remove ".*" -f

# 重试安装
conan install . --output-folder=buildDir --build=missing
```

### 未找到编译器

确保您的 C++ 编译器支持 C++20：

```bash
# 检查 GCC 版本
g++ --version

# 检查 Clang 版本
clang++ --version
```

### Meson 版本问题

```bash
pip3 install --upgrade meson
```

## 下一步

- [快速开始指南](/zh/user-guide/quick-start) - 学习如何使用 CLI
- [开发环境设置](/zh/contributing/development-setup) - 设置开发环境
- [API 参考](/zh/api/cpp-api) - 在您的应用中嵌入 ZYX
