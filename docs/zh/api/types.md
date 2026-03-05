# 类型定义

## 值类型

Metrix 支持以下数据类型：

- **空值**: `Value()`
- **布尔值**: `Value(bool)`
- **整数**: `Value(int64_t)`
- **浮点数**: `Value(double)`
- **字符串**: `Value(std::string)`
- **列表**: `Value(std::vector<Value>)`
- **映射**: `Value(std::unordered_map<std::string, Value>)`
