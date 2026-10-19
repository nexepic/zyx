# R1.2 continuous batch filtering and aggregation acceptance

This is the last foundational capability in the fixed R0.1/R0.2/R0.3/R1.1/R1.2 scope. Performance admission remains in R1.3/R7. There are no new formal timing samples or Kuzu-superiority claims.

## Implementation and replacement boundary

The native cell producer fills DirectPropertyColumns, reusing bound predicates, selection vectors, and BatchEnvelope. Borrowed string bytes feed comparisons and complete tuple/DISTINCT keys. Copy payload only for new groups or actual ownership needs, without constructing LogicalPlanReadRow/property_cache per input. Preserve primitive/mixed type identities and batch missing/stored-NULL distinctions with existing Cypher grouping/count mapping.

Reuse AggregateAccumulator, group state, DISTINCT sets, and owning output. Transfer group-copy/state charges through splitOff/absorb without refund/re-admission. Source decoding workspace and retained batches have separate reservations; new owner/token/directory staging is admitted before allocation. Accumulate in physical input ordinal order, not sorted lookup order.

Support native NodeScan and a single safe residual FILTER with property-left/literal-right comparisons and AND. Forward batch consumers through FILTER while indexed fallback rows still use the original Filter evaluator; do not bypass indexes or residual checks. Unknown providers, parameter residuals, complex/volatile/correlated expressions, complete-entity grouping, and COUNT DISTINCT entity retain bounded adapters for R1.3 auditing. Keep payload-free non-NULL probes where COUNT(property) has no value-consuming requirement.

Only genuine pre-delivery private construction pressure can split batches. Consumer failures never replay, and guard callbacks throwing MaterializedByteLimitExceeded propagate as guards. Preserve owner/incarnation, snapshot/schema/token-support identity, and envelope invalidation. No format, COW acknowledgement, isolation, resource cap, or scratch-default change.

## Validation and failures

- Initial run: 10 necessary modules, 257 cases, 254 passing and three failing; preserve first-run.json.
- Fix actual integration gaps: safe residual Filter forwarding and real BatchEnvelope publication. Add sealing, stale-borrow, guard-exception, and indexed-fallback protection.
- An old wide-value test required rejection at 16/32 KiB. Recompiled frozen c287 Runtime with matching current adapter/test ABI also accepts it, while the older monolithic binary passes. Preserve both controls; that binary is not current frozen-source evidence. Check empty-output semantics and add a real 1 KiB rejection, retaining malformed-column/retry checks.
- Final explicit selection: 75/75 passing, building 11 affected modules. Cover 24 residual comparison/type/NULL combinations, multi-key grouping, DISTINCT, physical floating order, private splitting, retry after errors, owned results, existing sort/join/OPTIONAL budgets, and reopen refunds. Initial plus targeted follow-up verifies 266 distinct cases, not one 266/266 run. No full unit suite.
- Native storage: 600 nodes across at least two batches; independently check country/active grouping, filtered COUNT/COUNT DISTINCT/SUM, input counters, and absence of legacy input-row materialization. Add an index and verify residual filtering with 43 matches per group. Result strings survive backend close.

proof.json, source-manifest.json, and tracked source-changes.patch identify changes from accepted R1.1; the commit/manifest also includes the new test file. run-necessary-modules.py reuses configured Ninja commands, builds listed modules, and executes an explicit filter. Arguments select build/output directories. Use the configured project toolchain; test duration is not benchmark evidence.

Accept R1.2 under R0's preregistered foundational contract. R1.3 and R2–R7 remain unaccepted. End-to-end performance, long-term space, and final Kuzu comparisons belong to their later stages.
