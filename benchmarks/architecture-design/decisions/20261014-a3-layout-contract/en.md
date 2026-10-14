# A3 adaptive property ownership and page-cost contract

Date: 2026-10-14. S1.3 design delivery. Keep the current production row layout. Existing laboratory carriers are rejected as production replacements; S3 integration/lifecycle/performance remains pending.

## Bounded grouping and unique authority

Separate Node/Edge ID spaces. Target group capacity is 64 logical IDs: nonzero ID maps arithmetically to group `1 + (id - 1) / 64` and slot `(id - 1) % 64`. Labels remain membership and never duplicate property groups or trigger entity movement. Logical IDs are not reused; physical slots retain generation checks. A live bitmap distinguishes deleted and absent owners. Sparse groups do not compact logical IDs merely to improve density.

Each group has ROW or MIXED mode. ROW retains canonical owner-keyed collections. MIXED owns column values exactly where column presence marks a live slot; a bounded row extension owns remaining keys. The owner enumeration lists the actual keys belonging to that entity. Column and extension ownership must be disjoint and complete; missing, stored NULL, and values remain distinct. No extension duplicates a complete canonical row. Point reads cross-check the selected ownership; full reads enumerate actual owner keys, not every group/database token. Mismatches are corruption, not missing-value fallback.

Initially permit integer/double/bool columns with presence/null/type information, compact dense or sorted sparse/rank storage. Nonfixed values, mixed-type exceptions and large values use bounded extensions and authenticated LargeValue references. Type changes do not create permanent schema. A selected scalar must not decode an unrelated large value. Large-value bodies remain immutable and separately routed; a small update must reuse unchanged references rather than recopy their bytes.

One shared group routing directory and shared packed pages serve low-density groups; no per-field/entity tree, per-owner root allocation or exclusive group tail page is assumed. Routing descriptors authenticate database, entity kind, group ID, birth generation, layout mode, live bitmap, enumeration/extension routes and column subblock locations. Locations are logical page ID plus bounded subblock identity, resolved through current page mappings. Do not store a full LargeValueRef per scalar column. Exact packed locator/codec widths require A7 review before assignment; no persisted format changes occur here.

## Access and COW paths

| Operation | Current row path | Target adaptive path | Changed pages |
|---|---|---|---|
| One entity/field | Token lookup, property-owner directory, property root, possibly whole collection LargeValue | Token, arithmetic group/shared directory, descriptor, owner-key membership, selected column or extension; authenticated page mapping | None |
| Full entity | Entity directory/page, labels, property-owner directory/root, all actual values | Same entity/labels; group descriptor plus actual owner-key enumeration, selected column subblocks/extension values | None; do not scan K unrelated fields |
| N owners/f selected fields | Sorted owner locations and property roots; large collections can read unrelated payload | Group range directory, bounded descriptor/presence reads and f selected columns per group, exceptions only when present | None |
| Set/remove/type change | Old property source, new collection, property page/directory, LargeValue/content/index metadata if applicable | Latest descriptor, membership and selected shared page/extension; preserve unrelated columns/large references | Touched data page, enumeration/descriptor pages, shared directory paths, semantic fence root, affected indexes and retirement metadata |
| Delete/reclaim | Entity/property/member/index updates, retired queue, allocator/catalog/page mapping | Live bitmap, owner enumeration, affected column presence/extension, group route only when empty, indexes and authenticated reachability retirement | Bounded touched pages/paths; physical reclaim follows old-pin release |

All routes are resolved under one metadata snapshot and owner/generation checks. Property mutation must advance the included state-directory semantic epoch or an existing property fence root even when stable logical pages only change mapping: the current seven-root graph fence excludes page/index-root directories. Publication atomically installs canonical ownership, group route, derived indexes and retire records. Old pins retain old layout. Candidate roots held only by a laboratory caller are not authoritative graph routing. LVID differences are retirement candidates, not proof that no other live owner references them.

## Cost model including metadata

Let E be live entities, F total actual properties, K distinct group tokens, G allocated groups, f selected fields, P=4096 bytes and h directory height. Group capacity B=64 bounds one group's work, not all values/field counts. Locator/routing storage grows O(G+number of touched shared pages); owner enumeration grows O(F), not O(EK). Column directory grows O(sum K) and is bounded by candidate admission. Presence/null bitmaps consume at least 16 bytes per column at B=64, plus explicit type/version/header bytes; dense fixed payload is at most 64 times scalar width. Sparse headers/rank metadata are included, not charged as payload. Extensions retain actual exceptional values and their enumeration/routing overhead.

Each owner token costs at least four encoded bytes before offsets/headers. A naïve 64-entry owner table in the current prototype costs 1056 bytes, and its compact membership directory for 64 memberships costs 592 bytes. The shared fixed field packed page is 576 bytes: 2224 bytes before container/native metadata, versus 3456 canonical owner-root bytes. Singleton fields become 4744 bytes before container/native metadata, already worse. Full LargeValueRef-per-column sparse carrier costs 4142 shared /16616 singleton encoded bytes and is rejected. The adaptive container chooses row fallback for singleton fields; its measured final serialized containers are 2296 shared /4016 singleton bytes, still not total allocated space.

Allocation metadata includes page-directory entries (32 bytes each in 64-byte-header 4 KiB leaves), catalog entries (48 bytes), allocator radix/summary, shared-pool bitmap/header, metadata routes, retired queues and A/B superblocks. Index root routing, token registry, content index and query indexes are separate. Physical pools reserve 128 KiB segments with 31 data frames: partial occupancy and retired retained pages dominate some small samples. COW write cost is rounded touched pages plus directory/allocator/index paths and publication fences, not changed scalar bytes. Point cost is O(h + owner-membership lookup + selected page reads); full read O(h + actual owner fields + their page reads); scan O(directory ranges + Gf + selected exceptions). Update includes affected indexes and O(h) shared paths; deletion also includes actual fields/degree and deferred reclaim.

## Admission, bounded work and lifecycle

Conversion is explicit and foreground-bounded, not background automatic tuning. Admit only when estimated fully allocated total (routes, descriptors, enumeration, bitmaps/types, data, indexes, allocator and retained old generation) improves the current row layout without violating point/full/update budgets. Use general byte/occupancy costs, never benchmark names, labels or sample-specific entity counts. A group with random interleaved shapes, singleton keys, repeated type changes or sparse post-deletion occupancy can stay ROW. Re-encoding/splitting touches at most 64 owners and configured field/byte limits; large bodies are referenced. Proposed limits: 4096 total group keys and 1 MiB planning bytes, additionally capped by A6 accounts. Exceeding any limit keeps ROW or fails the requested operation's existing admission, never drops fields. Repacking moves bounded subblocks and updates authenticated descriptors atomically; it does not change public IDs.

Conversion stages the new ownership and derived indexes, verifies complete enumeration, publishes one generation, and records retire candidates only after shared reachability checks. Reverse conversion follows the same protocol. Empty columns/subblocks/groups and replaced descriptors retire with exact owner/generation; reuse waits for every relevant old pin. Fault before publication leaves old complete graph; uncertain acknowledgment requires reopen. Migration, checksum-valid ownership disagreement, large-reference lifetime, relocation and reclaim require native graph-attached tests before S3.2/S3.3 acceptance.

## Actual page/byte evidence and limits

New native tests execute 15 production paths: 64 shared scalar entities; 64 entities each with a common scalar plus a unique field; and 64 entities with 18 fields including a 16 KiB string. Reopen before each path; 64 KiB raw-page pool, existing open-time catalog/schema loading excluded. Instrument successful positioned IO, classify authenticated page owners and metadata magic/directory types, and count unique physical 4 KiB page ranges. These are IO/correctness observations, not latency or device/RSS measurements. Read calls include repeated metadata work; unique page counts do not. Metadata subcategories overlap their parent totals and must not be added twice. No background physical reclaim is forced by `delete_retire`.

| Shape/path | Read bytes / unique pages | Write bytes / unique pages |
|---|---:|---:|
| Shared point | 77824 /19 | 0 /0 |
| Shared full | 94208 /23 | 0 /0 |
| Shared 64-owner one-field scan | 81920 /20 | 0 /0 |
| Shared single-field type change | 405504 /32 | 45632 /13 |
| Shared delete with retire publication | 847872 /66 | 202752 /55 |
| Singleton point/full/scan | Same respective shared results | 0 /0 |
| Singleton type change/delete | Same respective shared results | Same respective shared results |
| Wide/large point | 122880 /30 | 0 /0 |
| Wide/large full | 139264 /34 | 0 /0 |
| Wide/large scan | 3022848 /425 | 0 /0 |
| Wide/large type change | 1069056 /85 | 161536 /45 |
| Wide/large delete | 987136 /88 | 202624 /56 |

Shared point includes property 4096, index 4096 and metadata/control 69632 bytes. Wide point adds 20480 LargeValue data and 4096 manifest bytes; full reads add entity/label work. Wide scan reads 1310720 LargeValue data, 262144 manifest, 16384 property, 4096 index and 1429504 metadata/control bytes. Shared type change writes 4096 property plus 41536 metadata/control; wide type change also writes 20480 large data, 4096 manifest and 4096 index. Raw evidence separately records entity/property/page/index-root/retire/allocator directory costs where those codecs are encountered. This confirms current large-collection amplification and warns that metadata can outweigh payload.

Two additional fresh-store laboratory tests measure native adaptive-container IO for shared and singleton cases, including point/full/scan and whole-container set/remove rewrites. They include LargeValue manifests, content index, allocator and publication IO but omit production graph routes, derived graph indexes and durable retirement. Therefore they are a lower-bound experiment, not an end-to-end improvement ratio. Whole-container rewrite, loading all columns for a point read, and unretired old roots reject this carrier as production. The proposed bounded shared-subblock design must be tested separately. S1.3 accepts this complete design and explicit rejection; S3 remains pending, and final latency/steady-state/Kuzu measurements stay in the final window.
