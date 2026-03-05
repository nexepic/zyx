# Type Definitions

## Value Types

Metrix supports the following data types:

- **Null**: `Value()`
- **Boolean**: `Value(bool)`
- **Integer**: `Value(int64_t)`
- **Float**: `Value(double)`
- **String**: `Value(std::string)`
- **List**: `Value(std::vector<Value>)`
- **Map**: `Value(std::unordered_map<std::string, Value>)`
