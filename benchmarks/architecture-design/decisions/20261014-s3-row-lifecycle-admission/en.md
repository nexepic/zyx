# S3.2 native property lifecycle prototype and admission

The A3 container-column experiment remains rejected: it loads/replaces an entire container and has no authoritative graph route or durable owner retirement. No MIXED/column-group root is activated. The approved alternative retains the existing owner-keyed ROW authority and its actual-key enumeration, selected reads, COW publication, indexes and snapshot-safe LargeValue retirement.

A native opt-in ROW prototype now isolates large entry bodies from adjacent small fields using bounded logical chunk cut points. Canonical property bytes and their sorted token/offset directory remain identical. A large entry's token and six-byte value header stay in the preceding small group, so presence/non-null probes need not load its body. Adjacent small fields share chunks. Existing Manifest offsets/lengths describe these variable chunks; no new persisted root, codec, namespace, version or redundant complete row is introduced. The LargeValue layer remains domain-neutral and validates at most 4096 ordered nonzero cut points before any publication. Streamed data and multilevel Manifest materialization use actual ranges rather than ordinal-times-extent assumptions.

`PropertyRowChunkPolicy` considers RAW roots with at most 2048 fields, large entries at least one existing 16 KiB extent, and no caller-supplied cut policy. It admits only a smaller first-use chunk and at most 5% extra estimated encoded allocation. That estimate includes data/Manifest frames and current directory/catalog entry widths; shared-pool rounding, allocator/COW paths and retained versions are separately measured, not asserted free or universally bounded by this estimate. Compressed roots, small roots, very wide roots and failed cost admission keep the ordinary ROW plan without dropping fields. Metadata remains bounded by the count limits. The pre-existing write-admission/RSS boundary is retained; process-global resource coordination remains S5 work.

## Actual native lifecycle

The opt-in backend writes real graph Nodes through NativeGraphWritePlanner and PropertyCollectionRootStore. Tests cover small-field type changes with unchanged large bytes, stored NULL versus missing, remove, large-to-inline conversion, whole-entity deletion, bounded persistent reachability reclamation, old pins retaining old complete values, post-release retirement and reopen with the opt-in disabled. The new references use the same trusted owner route. Generic native tests additionally relocate a variable-chunk data frame while retaining old/current snapshots and the immutable reference generation, validate ranges crossing cuts and materialize a multilevel Manifest through the bounded streamed writer. Existing checksums, owner/generation checks, atomic data-before-root publication and retirement remain required.

One 256 KiB body plus scalar field was created independently in two actual native stores with the same 64 KiB raw-page pool. Each read path starts after reopen; open-time IO is excluded. Successful positioned IO and complete native file size include routing, descriptors, allocator/directory and data work. These are byte/space observations, not latency/device IO/RSS results.

| ROW placement | Scalar point read bytes | Non-null probe bytes | Complete Node read bytes | Complete file bytes |
| --- | ---: | ---: | ---: | ---: |
| Existing uniform chunks | 245760 | 237568 | 827392 | 2228224 |
| Cost-admitted boundaries | 180224 | 172032 | 843776 | 2228224 |

The full read does slightly more work; selected access saves 65536 read bytes in this case and pool rounding leaves file size equal. This does not establish a universal speedup or steady-state result. The singleton/small-root policy keeps existing ROW placement; the 16 KiB candidate that would inflate page cost is explicitly rejected.

Final validation passed **129 tests from six relevant suites**, including eight new boundary/native lifecycle cases, existing LargeValue compression/ranges/content identity/maintenance/corruption, property-store ownership/cache/validation and native integrity. Low maintenance limits that cannot retire one whole value fail before publication; the lifecycle test gives its 75-page value a finite 256-retirement cap. No full suite or performance benchmark.

Accept S3.2's lifecycle prototype and admission decision: retain ROW, approve conditional logical cuts, reject the unrooted whole-container column carrier. Native default activation and specification updates are S3.3; the dedicated final fault matrix is S3.4; O3/O5 latency/resource gates are S3.5. The backend opt-in remains disabled by default at this checkpoint.
