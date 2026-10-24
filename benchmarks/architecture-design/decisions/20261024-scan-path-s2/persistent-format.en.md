# S2 candidate B: complete file protocol (pending implementation)

This fixes the format/publication boundaries alongside `membership-wire.en.md`. It does not claim implementation or increase progress. Default 5/31 and experimental 13/39 retain their meaning; neither may contain the new scan membership records. Implement and test every requirement below before comparing candidate B.

## 1. Isolated versions and root

Add explicit, default-off `ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT`, requiring the existing hybrid option. This build uses SegmentEnvelope 15 and Superblock 41; ordinary builds remain 5/31 and old hybrid builds remain 13/39. Normal open accepts only its matching versions, without in-place migration. Keep the 352-byte Superblock header, the 16-byte hybrid descriptor reference at offset 336 and the 32-byte emergency reserve at 352.

Superblock flag `1 << 2` denotes BUILDING and is recognized only by 15/41. Public database open rejects a selected BUILDING root. Only the offline builder's private capability can access it; no public bypass option. Clear the flag only through final admission and atomic publication below.

Use hybrid descriptor version 2: retain the 32-byte frame header and CRC rules; expand the payload from 192 to 208 bytes. Preserve the first eight roots at payload offsets 64..191. The ninth root at 192 is a PageId u64 plus physical-pool generation u64, dedicated to node scan membership. Reserve internal index ID `0x8000000000000038`; it cannot appear in user schema index definitions. Do not reuse the original eight namespaces.

Payload offset 24 is live_node_count u64, 32 is membership_group_count u64, and 40..63 are zero. Counts describe the installed directory, which may be incomplete while BUILDING; final publication requires complete node coverage. Preserve database/semantic generation, physical frame generation, LSN, zero-tail and checksum authentication. Body CRC covers everything after the frame header, including zero tail. Header CRC zeroes its own field during calculation.

## 2. Directory, records and fields

Reuse the fixed-width B+Tree page protocol: an 8-byte big-endian group_id key and the existing 16-byte SlottedSlotReference value. Keys are nonzero, ordered and unique. Bind the root to the descriptor's database, generation and snapshot; a separately supplied tree root is not a certificate.

Each entry references `ScanBlockMembership` in a `POK_HYBRID_PROPERTY` page resolved through the same database's Page Directory. Validate owner, physical version, slot generation, record database/group, visible semantic generation and LSN. Generic hybrid page dispatch must explicitly recognize this magic and reject unknown records. CRC alone cannot replace the root-to-record authentication chain.

Store nonempty 64-ID groups covering every live node, including propertyless nodes. Never truncate a union exceeding 64 labels. Such groups require an explicitly encoded general-Entity fallback entry; until that encoding and read path exist, reject the entire construction on encountering them. Count refusals in adoption coverage rather than deleting inputs.

Reuse existing hybrid groups, columns, cold ROW and directory roots for fields; uncovered fields take general reads. Membership proof can remove duplicate Entity/label checks only for the same group and immutable root. Field owner, slot, type, length and accessed-page checks remain. Pass proof through a private scope binding database/data region/root/generation/fence/schema/I/O epoch, never a public boolean that disables validation. An indivisible value exceeding source workspace takes the existing query-budgeted owning path, is counted separately and is not typed-block adoption.

## 3. Separate destination construction

Accept only source 5/31; reject other versions before copying. Destination must be new and distinct from the source file. Open source read-only. Copy using at most one 128 KiB segment buffer, never whole-database memory materialization. Source valid A/B headers, file identity and length must remain stable before/after copying. On commit/replacement or failure to establish stable input, delete the private target and refuse. Neither source nor destination integrity may be skipped.

Copying preserves stable IDs, schema, indexes, vectors, LargeValue and ordinary records; changing version numbers alone does not establish a new layout. Controlled transcoding first authenticates the source version-31 magic/version, 336-byte header CRC, payload CRC, reserved bits and every original field. Then encode the same semantics under 41, with an empty hybrid root, emergency reserve moved from 336 to 352, BUILDING set and both CRCs recomputed. Do not accept arbitrary old versions or bypass original validation by patching bytes first. Reuse existing A/B selection/conflict rules. Write and synchronize BUILDING headers before copying the data region and remaining bootstrap bytes. Never copy publicly openable 5/31 source headers into the destination. Empty/partial/BUILDING targets cannot open as complete databases.

For allocated/retained segments requiring SegmentEnvelope, authenticate the complete source version-5 envelope and payload before re-encoding the envelope as 15 with a new header CRC. Leave internally byte-compatible pages/business records semantically unchanged. Do not promote free bytes to live data; reject any page kind/version whose compatibility is unverified. Preserve directory references and logical IDs instead of rebuilding benchmark data.

Build actual membership records and field layouts group by group only in the private BUILDING target. Preserve node/edge semantics and the existing public 8×64 per-operation conversion limits. Internal construction may persist progress, but every root remains BUILDING and unavailable through public open. Retain preparation/publication, cancellation, COW and reclamation contracts. Aggregate source/destination/temporary files must fit 2 GiB, block/source workspace 1 MiB each and maintenance workspace 8 MiB.

Before completion, run full integrity, whole-database node/membership counts and label/field oracles. Require at least 10% steady-space savings measured over the entire destination, plus construction-peak admission; final savings cannot excuse an excessive peak. Reject uneconomic conversion or inability to safely reclaim/compact. Only after all gates pass, flush data then clear BUILDING through one existing atomic A/B publication, returning success after durable acknowledgement. Never delete or overwrite the source.

## 4. Updates and recovery

Creation/deletion/label changes COW the affected membership and update live/group counts atomically with Entity, properties and descriptor. Property-only updates may reuse membership facts but must rebind batch proofs to the new root. Type changes/demotion retain general representations. Never update primary nodes first and membership asynchronously. Reject unsupported mutations before I/O/publication. S2 must support update/read/reopen; S3 completes all registered lifecycle cases.

Public open rejects BUILDING in the selected A/B root; it cannot silently fall back to an older complete root. Interrupted final publication either yields the complete committed root or a rejected BUILDING file, never an open partial graph. Do not auto-resume failed construction; use a new private destination. Old pins retain old descriptors/directories/pages through existing retirement. Relocation updates routing and root/fence identities; pointer proofs do not survive it.

Full integrity checks directory-to-physical-record ownership in both directions, rejecting orphan/duplicate/missing membership. Use bounded sorted merges with primary Entity IDs to compare live sets and complete label sets. Check field-directory consistency, owners, slots, generations, descriptor counts and visibility. Do not hide memory costs in a whole-database hash set.

## 5. Before implementing persistent writes

Verify version/namespace availability, unchanged 13/39 semantics, complete clone/transcode coverage for all retained segments, public BUILDING rejection, atomic refusal above 64 labels, integrity/retirement integration and safe rejection of unsupported writers. A membership codec alone does not satisfy this gate.
