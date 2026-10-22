# P4 label candidate access accepted

Progress 65%; all 85 affected module tests pass. No full suite or formal benchmark matrix.

Reuse existing label B+Trees, atomic token cardinalities and Entity Map. Node access compares record/index-key and page-validation byte work. Very sparse probes need no sample, all-node labels scan directly, and intermediate cases inspect at most 64 candidate locations. Counts and samples select paths only. Primary labels and all requested labels remain authenticated. Multi-label batches fit the existing 128-record session cache to avoid repeat decode. Temporary sampling containers are charged; admission fallback is allowed only before visitor emission.

One medium filter/real-group mechanism diagnostic each reduces 225000 scanned nodes to 100000 authenticated candidates/record decodes (64 sampled IDs on one page), with unchanged result oracles. A 1024/400 fixture decodes exactly 400 records; its dense label scans 1024. Coverage includes multi-labels, mutation/deletion, old pins, the production read-your-writes adapter, rollback, reopen, cancellation, statistics corruption and cursor bounds.

Expanded tests exposed an ordinary 8192-edge write retaining one ID capability per row. The batch coordinator now uses existing range reservations and reserved-ID creation; one acquisition covers the batch without raising limits or changing the durable high-water protocol. This is a P6 prerequisite and earns no P6 weight alone. Retained denial stacks are diagnostic evidence.

Prefix locality cannot establish whole-file fragmentation. Byte work is not fitted latency. P5 still needs to remove per-field positioning/decoding and intermediate representations; performance/Kuzu acceptance remains P7.
