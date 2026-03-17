# C API Reference

## `zyx_database_open()`

```c
METRIX_EXPORT zyx_database_t* zyx_database_open(const char* path);
```

Opens or creates a database.

## `zyx_database_close()`

```c
METRIX_EXPORT void zyx_database_close(zyx_database_t* db);
```

Closes and releases a database instance.
