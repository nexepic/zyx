# C API 参考

## `metrix_database_open()`

```c
METRIX_EXPORT metrix_database_t* metrix_database_open(const char* path);
```

打开或创建数据库。

## `metrix_database_close()`

```c
METRIX_EXPORT void metrix_database_close(metrix_database_t* db);
```

关闭并释放数据库实例。
