# C++ API 参考

## Database 类

### `open()`

```cpp
static std::unique_ptr<Database> Database::open(const std::string& path)
```

打开或创建指定路径的数据库。

**参数:**
- `path`: 数据库目录的文件系统路径

**返回:** Database 实例的唯一指针
