# CI 228: cold-process RSS attribution

## Scope and unchanged controls

This investigation uses production/test source `561460b30174e3dbc167f7a87bfceae6e1e7f138`, unchanged since its required unfiltered quick result of 11049/11049 enabled tests, six stress names each exactly once, and the full-project-instrumented native subset of 131/131. No production source, query allowance, RSS threshold, timeout, ASan quarantine/default, sanitizer check, or workflow was changed. No remote operation was attempted.

The separate cold ASan diagnostic remains a failure. The exact same unmodified O0 ASan+UBSan native executable that passed the 131-test group was rerun in a fresh process with only the sort selected. It failed its original 64-MiB RSS guard: baseline 191823872 bytes, rejection 263352320 bytes, growth 71528448 bytes (68.214 MiB). At rejection the sanitizer reported 5115314 live bytes and a 70004736-byte heap mapping. Its 4.659-second duration is time to rejection, not completion or a speed benchmark.

## Independent attribution

A separate diagnostic-only test translation unit is linked to the same verified 254-unit project sanitizer archive. It counts actual sanitizer allocation/free requests and samples one allocation stack per 256 calls, without allocating a tracking map. A QueryGuard sampler returns the unchanged actual process RSS; its first rejection sample pauses the child so an external supervisor can read `/proc/PID/smaps` before unwinding. Pause points and test-only hooks change layout/timing; they are not performance measurements. The unmodified executable independently reproduces the failure, so the diagnosis does not rely solely on a modified test.

At the diagnostic rejection, before exception cleanup:

| Measure | Before query | At rejection |
| --- | ---: | ---: |
| Live sanitizer allocations (bytes) | 351037 | 5112314 |
| Sanitizer heap mapped (bytes) | 21663744 | 70004736 |
| Anonymous mappings resident (KiB) | 29112 | 82956 |
| Diagnostic executable resident (KiB) | 46080 | 63104 |

During that interval, 112509 allocation requests totalled 37027539 bytes, and 97033 frees totalled 32266262 bytes. The exact difference, 4761277 bytes, equals the measured live-allocation increase. It does not equal the roughly 69-MiB whole-process RSS increase. The mapping deltas are 53844 KiB anonymous and 17024 KiB executable; libc/libstdc++ add only 4 KiB each. On this tmpfs-mounted executable its file-backed residency appears in `RssShmem`, so treating every `RssShmem` byte as a database buffer would be wrong. Actual mapping paths, rather than category names alone, establish the origin.

At rejection ASan reports approximately 35 MiB freed but zero really freed, a 9-MiB StackDepot with 31431 IDs, and primary/large allocator mappings. These figures overlap anonymous residency and each other; they must **not** be summed as independent contributors. They demonstrate retained instrumented allocation/metadata rather than 69 MiB of currently live query objects. After test/fixture/backend cleanup live allocations fall to 969612 bytes, while the sanitizer heap mapping remains 73822208 bytes and process RSS remains high. The residual live storage is not fully attributed; this does not prove the absence of every leak or the behavior of repeated queries.

Allocation stack samples identify normal execution-row tree nodes, sort-key vectors, bounded spill-record vectors, buffer-pool entries and scratch cleanup reads. Sampling is not an exact per-type total and includes cleanup after rejection. Existing changes already compact private row/cache structures, reuse spill decoding buffers, batch durable fragment disposal, and make collection accounting linear. The previously tested O1, streaming-disposal and read-buffer-reuse experiments did not establish a cold pass and remain unshipped. Another speculative container rewrite or warm-up would change accounting/performance/baseline semantics without evidence that it safely fixes this process-wide limit; none is introduced merely to obtain a green diagnostic.

## Normal-build control

The same frozen source and unmodified test pass in a fresh normal Clang19 Debug O0/g0 coverage executable (no ASan/UBSan), in 2.780 seconds with all original assertions/limits. A second normal execution sampled externally every 5 ms also passes; observed baseline RSS is 99631104 bytes and observed peak after its marker is 127098880 bytes, growth 27467776 bytes (26.20 MiB). This sampled peak is a lower bound and includes final cleanup/profile flush. The builds differ in instrumentation, executable composition/layout and run conditions; subtracting their RSS figures is **not** an exact measurement of ASan overhead or a controlled speed comparison.

## Coverage scope

The final required quick report is lines 95.85%, functions 98.15%, regions 93.89%, branches 86.89%. The repository's guidance requests 95% for all metrics, so that broader goal remains unmet. The new `ResultPropertyMapAdmission.hpp` is 100% on all four metrics in the full run, backed by independent actual allocator requests, cancellation and copy-growth tests. The checked local script and Build workflow do not enforce a numeric 95% failure condition; Build #228's reported failures were test assertions/timeouts, not a demonstrated coverage-percentage failure. No checked-in Codecov threshold configuration was found. Remote Codecov account settings were not inspected and cannot be inferred. Adding unrelated storage-wide tests to erase a full-repository branch gap is not treated as this CI root-cause repair.

## Remaining limitations

- Cold isolated ASan RSS is still red; the guard correctly measures total process residency, not merely live query allocations. It is an existing additional `continue-on-error` diagnostic, separate from the unchanged mandatory group. No check was weakened here.
- The observed native group now passes locally, but it is not the entire remote transaction/compaction/maintenance/process-crash gate.
- This environment provides Linux only. Apple libc++/mach resident-size and Windows MSVC/working-set behavior require native execution; neither is claimed from this evidence.
- A safe further production optimization has not been established. Changing sanitizer retention, warming away cold pages, changing the cap, or changing the meaning of the RSS guard would be a test/runtime policy change rather than a demonstrated allocation-lifetime fix. Those changes were not made.

Raw XML, allocation traces, mapping snapshots, source/build manifests and bounded diagnostic sources/scripts are in `evidence/2026-11-05/ci228-cold-rss-attribution`. Binaries, object files, profiles and user data are excluded.
