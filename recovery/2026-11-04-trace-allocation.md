# Avoid tracing allocations for repeated metric labels

The collector previously constructed `std::string(key)` on every duration,
value, or duration-batch update, including existing keys. Long labels therefore
caused one temporary allocation per event. The query-scratch RSS test explicitly
enables tracing; this unnecessary churn can contribute to instrumented process
memory but has not been proven to explain its entire macOS RSS increase.

The private sharded map now has transparent string-view lookup. It still owns a
string for every distinct label, retains its existing mutex, and keeps the public
Snapshot type unchanged. Existing keys require no transient label allocation.
No RSS limit, sanitizer flag, event, or assertion is removed.

Standalone Linux Clang19 -O1 probe intercepts operator new/delete, warms one label
and invokes all three update APIs 100,000 times each. Baseline 9256508a:
300,000 allocation requests / 10,500,000 requested bytes. Candidate: zero requests.
Both return identical totals (100,001 value events, 300,003 duration calls).
This is a single-thread allocation probe, not a query RSS or throughput benchmark.

Same-tree standard Ninja rebuild and focused binary: 27/27 tests pass, including
PerfTrace contracts, owned keys with embedded NUL and non-terminated views,
coordinated prefetch cleanup, and the unchanged 64 MiB RSS-guarded native sort/spill
case. macOS ASan/Windows still require native validation. No new CI was dispatched.
