# Native read session allocation ownership

Component implementation only: 22/34 accepted (64.7%); no new delivery is accepted.

Native snapshot/session bodies, cache indexes/FIFO queues, label buffers and all retained token-name copies now use the shared prepaid file cache grant. Existing adapter sessions do not construct unused fallback sessions. Standalone reads and adjacency cursors inherit their snapshot grant. Object-prefix ownership survives constructor failure and deletion through ordinary polymorphic unique pointers.

Coordinated Property caches retain canonical PMR byte buffers instead of untracked nested PropertyValue heaps. Canonical map ordering scratch uses the same resource; failed encoding restores the destination prefix. Legacy uncoordinated Property cache contracts remain intact. Returned decoded values still require caller/output admission. Encoding on retention and decoding on cache hits changes CPU cost; no performance claim is made before the final campaign.

Adopted schema vectors and nested strings reserve an audited capacity bound before ownership transfer. Small-string inline storage is conservatively counted; these credits are not PMR allocation measurements or RSS. Reservation does not allocate duplicate placeholder storage. Replacement destroys the former payload before refunding its former credit. Non-prepaid custom resources bypass this optional schema cache. Source schema decoding/argument construction remains a separate workspace gap.

Slotted page buffers and shared-pool copied payloads retain their allocation domain. Pool reads and retained Property cursor copies bind the snapshot grant. Read admission excludes already-owned page buffers, preventing a second query charge; unowned read temporaries retain their admission callbacks. An unnecessary pinned-page vector copy is removed. Integrity scan retained page payloads preserve their source arena; the complete integrity workspace and maintenance resource class remain pending.

Validation: 1,069 selected tests / 51 suites, zero failures; four isolated transaction OOM/terminal-retention workers pass with 1,408 bytes in each closed transaction object. Two expensive native sort integration cases were excluded from this selected run; external-sort/operator tests remain included. No full suite or performance campaign ran. XML, exact recipe/filter, build log, source hashes and worker evidence are archived.

Open work includes parser/compiler and native plan IR ownership, write staging/undo/overlay/COW workspaces, maintenance/recovery ownership and guaranteed progress under pinned cache pressure, partitioned hash/aggregate spill, Driver ABI value/serialization buffers, complete fault/long-run acceptance and final Kuzu comparison. These tests do not complete S5.3/S5.4/S5.5/S6.1/S6.3.
