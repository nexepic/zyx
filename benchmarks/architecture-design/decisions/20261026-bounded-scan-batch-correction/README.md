# One bounded corrective wave after failed S2 selection

Accepted progress remains **15%**. The corrective recheck is complete and failed the adoption gate. See [result.md](result.md) for all eight measured comparisons. This directory does not establish adoption.
The original comparison completed 30 primary processes and 1,200 measured
samples. Both candidates failed the frozen per-case 5x gate. Original artifacts,
source archive, raw workers and `selection-gate.json` remain under
`results/20261024-work-scan/final-comparison/`. S5 guards were stopped after the
upstream selection failure and are incomplete.

The existing contract permits one correction/recheck for demonstrated bugs.
Untimed original counters established that certified native membership was
actually active, while each field group restarted its directory lookup and
multi-field reads missed the grouped path. Ordinary insertion after conversion
also scanned 860,000 Edge directory pages because a managed insertion summary
was stale. These are implementation omissions in the chosen representation.

The correction adds a governed eight-entry directory window and one shared field
page view within the certified bounded batch, preflights multi-field native
columns before any output, and refreshes existing stale insertion summaries
before completion. Owner, version, I/O epoch, cancellation, cold-field fallback,
fresh full-integrity and actual whole-file admission gates remain in force.
There is no new persistent format or additional cache allowance.

Five new tests cover batch authority boundaries, mixed native/cold fallback,
I/O epoch invalidation, actual insertion summaries and bounded maintenance with
old pins. The three affected modules passed **83 tests, zero failures**; the
full unit suite was not run. Test counts from earlier overlapping runs must not
be added to this count.

The admitted medium/holdout originals are preserved. `refresh-derived.cpp` in
the preceding decision directory applies the existing derived-index maintenance
operation to fresh copies with 7 MiB work, aggregate file accounting and the
original 10% whole-file savings check. Both copies retain their exact file size;
the original source hashes are unchanged. This maintenance cost is recorded
separately from queries. Future converter completion performs the refresh before
its mandatory final full-integrity and prepared-file admission checks.

`check-work.py` performs untimed complete public result checks and captures work
counts. Once related compatibility checks and these oracles pass, `recheck.py
freeze` creates a new immutable artifact/source identity; `recheck.py run` runs
only corrected B against the unchanged initial/control/Kuzu engines, with the
same four cases, two datasets, three processes, three warmups and ten measured
samples. This is the single corrective recheck, not another full S5 campaign.
Original failed evidence and original freeze identity are never overwritten.

Any failed acceptance gate remains unaccepted; neither code volume nor local
correctness can justify 100%, a progress commit, or production adoption.
