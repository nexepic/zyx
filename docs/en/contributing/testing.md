# Testing Guidelines

## Unit Tests

Located in `tests/src/` matching the source structure.

## Integration Tests

Located in `tests/integration/` for end-to-end workflows.

## Running Tests

```bash
# All tests
./scripts/run_tests.sh

# Specific test
meson test -C buildDir <test_name>

# With coverage
./scripts/run_tests.sh --html
```
