# P2 acceptance: incremental ownership, capacity, and reclamation

Progress is 40%; P1/P2 accepted. All 298 affected module tests pass. No full unit suite or new performance matrix was run.

For one fixed change across 3/300/900 Catalog leaves: source route frames 2/4/5, ownership visits 6/6/6, capacity visits 8/14/13, ordinary full traversals zero. With 100 full pools and one available pool, capacity preparation, reservation and allocation inspect three candidates in total.

Retirement deltas cover copied path sources, replaced targets and private intermediate frames. Validation precedes A/B publication; allocation-free handoff queues embedded per-frame links afterward. The oldest pin delays reclamation. Missing coverage or a mismatched source requires complete reconciliation. Recovery and explicit full reconciliation reconstruct delayed retirement. Ownership is confirmed in its publishing transaction instead of deferring bulk work to the next small transaction.

Resident overhead includes 496 bytes of retirement links per 128 KiB pool and fixed intrusive ownership/capacity fields. No per-frame heap nodes or retained transaction arena. acceptance.json contains counters, source hashes and test evidence. checkpoint-25-percent.json is the earlier unaccepted checkpoint. End-to-end performance and Kuzu acceptance remain for P7; continue with P3 read costs.
