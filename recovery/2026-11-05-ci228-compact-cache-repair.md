# Build 228: compact query caches and budget ownership

This local work continues `9a4cca5d`. No remote branch or CI was changed. The
previous frozen `212f82a3` complete quick run executed all 11042 enabled tests
exactly once: 11019 passed, 23 failed, 20 disabled, no skip. Its complete XML,
source fingerprint and failures are preserved in `ci228-compact-rows`; they are
not results for the new source.

## Confirmed causes and changes

* Scheduling a batch from only `sizeof(LogicalPlanReadRow)` chose different
  carrier counts after layout changes and across STL implementations. The
  estimate now includes an allocated binding node, leaving real payload space.
* Compact, stable-address Boost.Container trees now store all private row
  bindings and nested caches on all platforms. Public result/PropertyValue map
  types and persistent encoding are unchanged. Core-only and WASM builds need
  Boost headers, not a Boost runtime library. The WASM CMake helper was checked
  independently: it exposes a Boost-only shim, not host libc headers. An actual
  Emscripten build is not available in this environment.
* Old prefetch increments counted logical key lengths and payloads but not
  owned tree nodes. Cell producers now reserve destination alias/key metadata
  before decoding. Metadata and payload credits transfer into the same query
  budget; failure returns each credit to its original owner. This does not use
  the independent backend CACHE domain. Legacy splits also reconcile the real
  row reservation before transferring it.
* Internal cache copies remain compact trees; they no longer round-trip through
  a temporary public hash map. Public map conversion remains explicit, guarded
  and charged according to the destination layout. Entity result accounting
  no longer charges an embedded map object twice; nodes, buckets and payload
  buffers remain charged.
* Converting assignment preserves the destination allocator and uses a
  replacement tree so allocation failure leaves the old map intact. A tracking
  allocator regression checks failure, retry, ownership and complete refund.

## Test boundary corrections

Limit/Skip exact-fit tests independently include the complete allocated binding
node and retained string heap, with a frozen 1792-byte efficiency ceiling. They
also verify that one byte less rejects before delivery, then retry at the exact
bound. No production quota is raised.

Several old success formulas scaled with empty row header sizes, inadvertently
shrinking the cap faster than live public result growth. Nested-sort node
results now use a fixed 4352-byte efficiency envelope: the independently
reconstructed pre-compaction GNU reference was 4758 bytes (row header 280,
PropertyValue 64, ancillary retained fees 646). CSV nested sort uses a fixed
2304-byte envelope, below its former GNU 2576-byte formula and below eight
independently owned binding rows. Prefetch-to-sort accounts for actual cache
nodes and stays below 1408 bytes and below duplicate source/output payload
ownership. These are reviewable fixed gates, not bounds automatically increased
from measured peaks. Complete order, one fetch per owner, cancellation and
refund assertions remain.

Training publication failure now consumes the account's remaining headroom in
the existing prepare hook. It checks a real post-prepare denial, no maintenance
commit, refund and retry rather than assuming a 1024-byte cap must always fail
after layout optimization. The projected shortest-path rejection uses the
independent minimum live path/vector plus three rows of binding nodes, instead
of claiming the now-sufficient 4096-byte quota necessarily omits source fees.

## Current verification and limits

The standard Ninja core/test build completed successfully. That binary passed
31/31 selected tests: all 23 previous failures and eight allocation/container
regressions. Raw XML is in `ci228-compact-cache`. Earlier intermediate 14/23,
18/23 and 21/23 results are kept separately, never rewritten as passes. The final
unfiltered required quick run is still pending at this checkpoint.

The independent GNU/libc++19 component runs each passed 226 tests; they preceded
the final allocator-assignment regression and are not whole-database or native
Windows/macOS results. The final standard binary includes that regression.

The real 64 MiB RSS cold probe remains failing. Current partial-ASan instrumentation
reported peak live allocation 7976266 bytes and RSS growth 67710976 bytes. Large
allocation sampling identifies real scratch/segment preparation buffers, record
vector growth and buffer-pool entries; it does not establish macOS allocator or
quarantine attribution. The 64 MiB limit, 120-second query timeout and sanitizer
settings were not relaxed. Neither this probe nor a diagnostic larger envelope
is a pass for the original gate. Native Windows/macOS/full ASan CI is unverified.
