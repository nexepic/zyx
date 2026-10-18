# Native benchmark process peak RSS

The old read-window RUSAGE_CHILDREN includes provenance/launcher helpers; POSIX rusage can also retain pre-exec parent peaks. Preserve those observations and do not treat them as isolated native-engine peaks or retroactive resource acceptance.

The optional ZYX_COMPARE_PROCESS_MEMORY_REPORT writes a separate JSON after all query timers. Linux reads post-exec VmHWM, Windows uses PeakWorkingSetSize, and other POSIX rusage is explicitly labelled as possibly including pre-exec peaks. Default JSONL remains unchanged; existing report paths are rejected. Add standard Windows psapi linking. Both cores use identical benchmark-entry source.

Four real tiny CLI checks pass: current, original control, default no-report, and overwrite rejection. With a deliberately resident~282MB Python parent, legacy child rusage reports~300MB while native processes report~17.8MB/12.3MB. This verifies capture only, not memory/performance wins. Scope includes native setup/output, not just query workspace. Non-Linux branches were not executed here.

The first verifier omitted stderr error events; preserve its log and verify both JSONL streams in a fresh directory without weakening assertions or changing C++ behavior. Progress31/34 (91.2%) unchanged.
