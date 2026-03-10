# C API Reference

## `metrix_database_open()`

```c
METRIX_EXPORT metrix_database_t* metrix_database_open(const char* path);
```

Opens or creates a database.

## `metrix_database_close()`

```c
METRIX_EXPORT void metrix_database_close(metrix_database_t* db);
```

Closes and releases a database instance.
