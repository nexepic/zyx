# 编写测试

全面的测试对 Metrix 的可靠性至关重要。本指南解释如何编写有效的测试。

## 测试理念

Metrix 遵循以下测试原则：

1. **测试行为而非实现**：关注代码做什么，而不是如何做
2. **高覆盖率**：所有指标目标 95%+ 覆盖率
3. **测试边缘情况**：边界条件、错误情况和无效输入
4. **保持测试独立**：每个测试应该是隔离和可重复的
5. **使用描述性名称**：测试名称应清楚地指示它们测试什么

## 测试结构

### 基本测试文件

```cpp
// tests/src/core/test_Database.cpp
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"

using namespace metrix;

class DatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDbPath_ = createTempPath();
        db_ = Database::create(testDbPath_);
    }

    void TearDown() override {
        if (db_) {
            db_->close();
        }
        removeTempPath(testDbPath_);
    }

    std::string testDbPath_;
    Database* db_ = nullptr;
};
```

## 单元测试

### 测试单个方法

```cpp
TEST_F(DatabaseTest, ExecuteQueryReturnsCorrectResult) {
    // Arrange
    std::string query = "CREATE (n:Person {name: 'Alice'})";

    // Act
    auto result = db_->execute(query);

    // Assert
    ASSERT_FALSE(result.isEmpty());
    EXPECT_EQ(1, result.getStatistics().getNodesCreated());
}
```

## 集成测试

### 测试完整工作流

```cpp
TEST_F(IntegrationTest, CompleteCrudWorkflow) {
    // Create
    db_->execute("CREATE (n:Person {name: 'Alice', age: 30})");

    // Read
    auto result = db_->execute("MATCH (n:Person {name: 'Alice'}) RETURN n");
    ASSERT_FALSE(result.isEmpty());

    // Update
    db_->execute("MATCH (n:Person {name: 'Alice'}) SET n.age = 31");

    // Delete
    db_->execute("MATCH (n:Person {name: 'Alice'}) DELETE n");
}
```

## 测试异步代码

### 测试并发操作

```cpp
TEST_F(ConcurrencyTest, MultipleThreadsCanRead) {
    db_->execute("CREATE (n:Person {name: 'Alice'})");

    const int numThreads = 10;
    std::vector<std::thread> threads;

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([this]() {
            auto result = db_->execute("MATCH (n:Person) RETURN count(n)");
            EXPECT_EQ(1, result.peek()["count(n)"].asInteger());
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}
```

## 性能测试

### 基准测试

```cpp
TEST_F(PerformanceTest, LargeGraphQuery) {
    const int numNodes = 100000;
    for (int i = 0; i < numNodes; ++i) {
        db_->execute("CREATE (n:Node {id: " + std::to_string(i) + "})");
    }

    auto start = std::chrono::high_resolution_clock::now();

    auto result = db_->execute("MATCH (n:Node) RETURN count(n)");

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_LT(duration.count(), 1000); // 应在 < 1 秒内完成
}
```

## 最佳实践

### 1. 使用 AAA 模式

```cpp
TEST_F(ArrangeActAssert, Example) {
    // Arrange：设置测试数据和条件
    std::string query = "CREATE (n:Person {name: 'Alice'})";

    // Act：执行被测试的代码
    auto result = db_->execute(query);

    // Assert：验证预期结果
    EXPECT_EQ(1, result.getStatistics().getNodesCreated());
}
```

### 2. 测试一件事

每个测试应关注单一行为。

### 3. 使用描述性断言

```cpp
EXPECT_EQ(expected, actual) << "节点计数应该匹配";
ASSERT_TRUE(found) << "未找到 id 为 " << nodeId << " 的节点";
```

### 4. 清理资源

始终在测试后清理资源。

## 运行测试

### 运行所有测试

```bash
./scripts/run_tests.sh
```

### 运行特定测试

```bash
meson test -C buildDir test_Database
```

### 运行覆盖率

```bash
./scripts/run_tests.sh --html
```

## 另请参阅

- [测试指南](/zh/contributing/testing) - 整体测试策略
- [代码规范](/zh/contributing/code-style) - 编码标准
- [开发设置](/zh/contributing/development-setup) - 入门指南
