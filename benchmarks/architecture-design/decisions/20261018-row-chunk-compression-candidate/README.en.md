# ROW chunk compression candidate

Wide-property diagnosis confirms that a small-field update still materializes its row's large payload. After existing ROW page-cost admission, try the existing immutable LZ4 chunk codec with the unchanged 5% minimum savings, RAW fallback, logical cuts and page-cost gate. Only select it when LZ4 is available and RAW fallback is allowed. Explicit codec/cut policies and disabled adaptive placement retain their behavior. No new format, Driver ABI, resource or deadline change.

All 680 tests from 49 necessary modules pass, including three new cases covering explicit/strict RAW policies, actual compressed page counts and canonical values, and incompressible RAW fallback/page bounds. Related snapshot, relocation, reclamation, recovery and type-change modules pass. No full unit suite.

All 886 checks across 13 smoke families and the same 578 diagnostic operations pass. The 128-field/1MiB diagnostic updates reduce median write bytes from 2736288 to 449696; the final diagnostic file falls from 44695552 to 9830400 bytes. Both pre-repair static readonly consumers reproduce complete wide/large/dynamic values from the new compressed file without changing its hash.

This establishes mechanism and necessary correctness only. Full dynamic10000 and wide/large contracts must still finish under the original1800s,2GiB file,6GiB AS and64MiB query limits before final read/write/resource gate review. Frozen candidate pending acceptance; progress31/34 (91.2%), no claim of outperforming Kuzu.
