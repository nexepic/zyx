# S2 acceptance after the explicit user instruction

On 2026-10-26 the user explicitly removed the fixed 5x requirement ("不需要一定按照文档要求提升5倍"). This is a user-authorized change to the active acceptance rule, not a rewrite of historical failed measurements. Original freezes, raw receipts and earlier failure reports remain unchanged.

S2 is accepted at **35% total progress**: corrected B improves every primary case on both frozen datasets against both current-machine controls; all complete oracles pass; actual whole-file savings are 17.25% and 14.57%; bounded construction and source/maintenance contracts are retained. The existing 24-process/960-sample window is reused without running another benchmark. Current engine source and production library match its recorded hashes.

This acceptance covers candidate selection and its implemented persistent scan path, not S3–S5. The conversion is expensive (about 207 minutes medium and 136 minutes holdout on the recorded machine), and is explicitly offline rather than free query preparation. Kuzu is still faster in all eight primary comparisons; superiority and 100% are not claimed.

83 scan/build, 64 compatible-read and five default-insertion related tests passed. No full unit suite was run. Full production lifecycle delivery, abandoned A cleanup and the remaining regression/total-cost checks remain the next authorized work. Historical documents continue to describe the gates that applied at their measurement time.
