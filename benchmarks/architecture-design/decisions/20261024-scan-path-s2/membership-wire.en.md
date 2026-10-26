# S2 persistent scan membership: byte protocol

This fixes the membership record encoding alongside `persistent-format.en.md`. A valid record alone grants no Entity authority: matching file versions, complete source coverage, atomic publication, update/recovery rules and whole-file admission remain mandatory. The isolated implementation has module evidence; current lifecycle and adoption status are recorded in the bilingual optimization checklist and decision checkpoints. S2 remains unaccepted at 15% pending real-image and final performance gates.

Reuse the existing physical group's 64-ID interval, within S1's 512-row batch limit. group_id = 1 + (entity_id - 1) / 64; bit i denotes (group_id - 1) * 64 + 1 + i. The final group must not set bit 63, which would overflow uint64. Membership covers every live node, including nodes without properties or labels, not just owners with columnized fields. A union exceeding 64 labels sends the entire group to general reads; never truncate labels or treat unrecorded labels as absent.

All integers are little endian. A 64-byte header precedes up to 64 entries of 16 bytes. Exact length is 64 + 16 * label_count, at most 1088 bytes.

| Offset | Field |
|---|---|
| 0 / 4 / 6 | magic u32 = 0x4d53595a / version u16 = 1 / header_bytes u16 = 64 |
| 8 / 16 / 24 | database_id u64 / semantic_generation u64 / group_id u64 |
| 32 / 40 / 42 | live bitmap u64 / label_count u16 / reserved u16 = 0 |
| 44 / 48 / 52 | total_bytes u32 / body_crc u32 / header_crc u32 |
| 56 | reserved u64 = 0 |
| 64 + 16*i | label token u64 followed by membership bitmap u64 |

Use existing calculateCrc. The body checksum covers all label entries. The header checksum covers all 64 header bytes, including body_crc, with its own field zeroed. Label tokens must be nonzero and strictly increasing. Each label bitmap is nonempty and a subset of live. Validate the complete directory before selection. An empty predicate returns live, multiple labels intersect bitmaps, and an unknown token produces an empty result. Reject malformed/missing/duplicate/unordered fields, reserved bits, wrong lengths/checksums, mismatched database/group and future generations.

CRC and semantic_generation alone do not prove visibility. Production integration must resolve the current snapshot's authenticated root to its actual owner page and authenticate physical version, slot generation, LSN, database/data-region identity and I/O epoch. Retain the owner throughout any borrow. Membership, node, label and field changes must publish together; old pins retain old records. Until these invariants are implemented, the prototype cannot replace primary node existence or label checks.

Tests cover intersection and unlabeled nodes, every-byte mutation, all truncation lengths, wrong database/group/generation, duplicate labels, nonlive members, label-count limits and the uint64 ID boundary. Tests manipulate memory bytes only and create no candidate-format files.


2026-10-25 implementation addendum: normal v1 encoding is unchanged. The explicit v2 primary-label fallback for over-64-label groups is specified in `../20261025-membership-label-fallback/format.en.md`; resource and adoption gates are unchanged.
