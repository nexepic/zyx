# S5.3 single-file spill and budget integration acceptance

Supported native paths share process/database QUERY, CACHE, WRITE and MAINT authority. Sort runs/merge and partitioned DISTINCT/UNION use registered single-file scratch. Unsupported aggregate/hash-join spilling fails explicitly. Existing S5.4 cancellation, disk-pressure, readonly, concurrency and crash-cleanup evidence and S6.3 soak evidence remain separately identified.

Procedure rows, property/metadata caches and output construction now admit heap requests before allocation. Local expression reservations transfer to rows and then to owning QueryResult output without a refund/readmission gap. Freed PMR buffers refund normally. Cross-batch DISTINCT keys retain independent ownership. Vector-page backing, native delta scans and returned hits retain allocation authority; immutable record reads borrow the live page instead of making temporary copies.

All 1,979 tests in 136 suites from 95 explicitly selected affected modules pass. Every observed exact-vector search admission point (217) rejects, refunds and retries successfully. New native procedure tests cover scalar, full node, DISTINCT, Top-K, labels, properties and projected variable outputs. Four Kuzu resource-configuration tests also pass. No full unit suite or new performance campaign ran.

S5.3 implementation/resource correctness is accepted; S5.5 steady-state performance remains pending. Legacy raw owning API output buffers are caller-owned boundaries, not managed internal paths. Runtime verification is Linux only; Windows/macOS layout and move/proxy lifetime constraints are implemented but not claimed tested. Charged allocation accounting is separate from RSS.
