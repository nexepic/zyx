# A2 unchanged property patch elimination

Date: 2026-10-13. Partial implementation and selected tests pass; S3.1/S3.5 were not accepted by this change.

Before normalizing a patch into a complete property collection, the logical write adapter compares sorted snapshot fields with SET/REMOVE operations. It skips the replacement when all SET values compare equal under the existing PropertyValue contract and all removed keys are absent. The overlay consequently preserves its revision and avoids unnecessary root/index preparation. Unequal values, including NaN under that contract, retain the original path. Actual changes still use complete collection encoding and the same publication protocol.

The added regression checks unchanged properties and revision. Twenty selected patch/backend tests passed, covering native creation, reopening, admission retries and unique-constraint rollback. Build prerequisites also fixed explicit reservation aggregate construction and the BTree-page test's missing `<algorithm>` include. Historical evidence and source hashes are in `benchmarks/architecture-design/results/20261013-a2-noop-property-patch/`.

No benchmark was run; reduced replacement work is not an end-to-end speedup claim. This partial change added no acceptance item and did not alter the fixed denominator of 34.
