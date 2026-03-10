# C++ API Reference

## Database Class

### `open()`

```cpp
static std::unique_ptr<Database> Database::open(const std::string& path)
```

Opens or creates a database at the specified path.

**Parameters:**
- `path`: File system path to the database directory

**Returns:** Unique pointer to Database instance

### `beginTransaction()`

```cpp
std::unique_ptr<Transaction> Database::beginTransaction()
```

Begins a new transaction.

**Returns:** Unique pointer to Transaction instance
