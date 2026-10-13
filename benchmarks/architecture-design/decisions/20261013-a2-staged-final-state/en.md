# A2 bounded final-state property and label staging

Date: 2026-10-13. Transaction accumulation now retains the last replacement per full entity reference. 127 selected module regressions pass. S3.1 is not fully accepted.

Previously, `append` retained each statement's complete property/label replacement until final canonicalization. Repeated wide-property changes grew the payload with statement count, and subsequent failure-safe candidate copies duplicated this history.

`mergeFinalReplacements` uses entity kind/reference kind/value to replace earlier entries in the candidate accumulated state. Failure preserves the original state; success moves the candidate into place. The overlay retains each statement's intermediate visibility. Existing creation fold/rebase rules remain, and Node/Edge IDs are not conflated. Independent transactions and durability barriers are not merged or reduced.

Diagnostics record accumulated target counts before publishing the candidate, so diagnostic allocation failure cannot occur after transaction mutation. A new regression performs 32 replacements of a 16 KiB owner's properties and labels, checks the visible rank/label after each statement, verifies both retained-target maxima are 1, and checks the final payload/rank/label after commit. This measures accumulation bounds, not total query memory, RSS, allocations or latency.

The current production core and affected module objects pass 127 tests with no failures, including Node/Edge final states, creation references, failed-statement isolation, indexes/constraints, old snapshots, real Cypher transactions, indeterminate final sync and DETACH behavior. No full suite or benchmark was run. Evidence is in `benchmarks/architecture-design/results/20261013-a2-staged-final-state/`.

Complete collection staging and candidate copies remain. Native field staging/accounting and adapter replacement continue; S3.1 and performance acceptance stay open. The newly accepted item is the separate S1.7 format design: S0 25%, S1 28.6%, S2–S6 0%, total 3/34 (8.8%).
