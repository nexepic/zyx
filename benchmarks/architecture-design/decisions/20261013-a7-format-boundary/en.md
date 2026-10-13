# A7 format versions, stable commitments and migration release boundaries

Date: 2026-10-13. This is the S1.7 design deliverable. The production format remains developmental. No migrator, layout switch or stable-format release date is implemented or announced.

## Current identity and compatibility

`StorageBackendFactory::create` probes existing files read-only. `StorageFormatProbe::inspect` reads at most two fixed 4 KiB bootstrap slots, and `SuperblockFile::recover` selects a supported valid generation. Segment envelopes are segment-level boundaries, not a universal header at the beginning of the file. Local readers validate their own versions, identities, lengths, flags and checksums when reached; opening is not a complete integrity walk.

The current envelope version is 4 (64 bytes). Superblock version is 30 (4096-byte page, 336-byte header). Version fields are u16 little-endian and belong to independent namespaces. `version-profile.json` inventories 42 explicit production version constants with their declaration hashes. Wire layout comes from explicit codecs, never C++ structure size/padding. Relevant local versions include EntityPage 4, PageOwner/SharedPagePool 2, BTreePage 3 and packed/entry 1, Property root 2 and LargeValue payload 3/reference 1, LargeValue manifest 2/data page 1, and Vector manifest 8/page 3/codebook 3. Catalog physical radix 4 and logical radix 5 are distinct interpretation layers. Known typed-bootstrap flags retain their implemented reader branches.

The actual compatibility matrix is:

| Case | Current behavior |
|---|---|
| Superblock 30, valid known flags and reachable current local encodings | Current readable profile; writing also requires the existing mode/fence/identity/pin contracts |
| Envelope 4 | Its segment may be decoded after kind/state/size/checksum validation |
| Other envelope version | Envelope codec rejects it without reinterpreting offsets |
| A single non-30 Superblock | Direct codec reports unsupported version |
| No supported valid A/B slot | Probe reports incomplete native; factory refuses open. Pair/select reports checksum mismatch when neither slot is valid |
| One supported valid slot and one invalid/unsupported slot | Existing recovery selects the valid slot; this is recovery, not an old/future-profile migration promise |
| Valid slots disagree at the same generation | Invalid-state split-brain rejection |
| Valid slots have different generations | Select the higher valid generation |
| Unsupported reachable local version/flags/type | Reject at the corresponding reader/integrity visit; exact errors depend on codec and validation order |
| Known typed bootstrap versus ordinary radix | Use only its own bootstrap/route interpretation |
| Old magic, unrecognized, truncated or empty existing file | Refuse open without automatic conversion, deletion or truncation |
| Benchmark layout prototype/carrier | Not an adopted production profile |

These are specific supported combinations, not a Cartesian product of local versions. Checksums do not prove structural/cross-structure consistency or detect every unvisited corruption. A future public reader/writer profile list must be explicit; recovering a known slot does not establish forward compatibility.

## Registration and stable release gates

A1/A2 memory/request/staging changes add no persistent root or encoding. A3/A4 production encodings, A5 conflict metadata and A6 scratch roots are not finalized. Before writer integration, each adopted change must register: root/catalog ownership and logical/physical identity; exact offsets/widths/endian/length/presence/null/type/flags/checksum/version rules; first-read authentication and consistency/integrity discovery stages; all readers/writers/import/maintenance paths and atomic reachability; failed candidates, pins, retirement/relocation/slot reuse and child/LargeValue reclamation; reader/writer matrices, rejection/recovery tests and adoption evidence. Use existing namespaces/allocator/COW/A-B publication and prefixed enums. Never silently reinterpret old bytes or rely on old readers falling back to an earlier generation. Incompatible profiles are created in new files, not mixed into one A/B pair.

Library version, public Driver ABI stability and persisted-format stability are separate commitments. A first stable-format promise requires all adopted A3–A6 protocols to be frozen; final S6 source/correctness/fault/recovery/resource acceptance; complete registration, golden bytes, corruption/unknown-version and cross-platform rejection evidence; aligned specifications; explicit stable-profile identity/lifetime/change policy and owners; and the independently tested migration protocol below, or an explicit statement that the first stable profile has no older stable profile to migrate. Development data must never be deleted automatically to satisfy release gates. Compatible extensions require proof for every supported old reader/writer combination. Physical plan/schema identities remain separate from file and ABI versions.

## Chosen migration method before a stable release

Use an explicit offline streaming copy to a new database, not in-place overwrite or automatic migration during open. The user supplies a separate output path; the source opens physically read-only with no concurrent writer. Preserve logical entity IDs, endpoints, parallel-edge multiplicity, labels, property presence/NULL/type/value, constraints and public semantics. Give the target a distinct DatabaseId. Rebuild and verify derived indexes/representations from authoritative entities rather than copying pages under a new interpretation.

Build a task-owned, unpublished target database. Each database stays single-file; persistent migration progress/recovery, if introduced, belongs inside the target rather than a source WAL/SHM/external journal. Precheck profiles, paths, permissions, exclusivity, memory and source-plus-target/build/COW space bounds. If space cannot be bounded reliably, enforce a hard quota and abort target construction on exhaustion while leaving the source unchanged.

Phases are PRECHECK → bounded BUILD → VERIFY (complete integrity, IDs/endpoints/members/exact counts, canonical typed values, constraints/index/vector consistency) → DURABLE (target publication and sync) → PUBLISH (same-filesystem promotion to a previously absent output path with directory durability). The source path is not replaced by default. Loose `PropertyValue ==` is insufficient to prove lossless NaN/signed-zero migration.

Precheck/build/verification failures leave source bytes unchanged and never publish the target; cleanup applies only to verified task-owned unpublished files. A crash never automatically promotes an unverified build. Uncertain promotion/directory-sync returns indeterminate and requires reopening and full verification; the source remains available. Cross-platform rename/directory-sync, disk-full, short-write, cancellation and crashes in every phase are independent release tests. No migrator was implemented or run in this task.

## Design acceptance boundary

The current source inventory, opening/codec chain and selected format tests substantiate the matrix. A new test fills A/B with checksum-valid old/future bootstrap versions and verifies refusal without changing file length or bytes. Exact pass counts/hashes are recorded in results/verification. No full suite or benchmark was run.

S1.7 accepts this complete design, release prerequisites and version boundary. It does not accept a migrator, stable-format release or unadopted layouts. Concrete A3/A4 codecs and migration fault coverage remain mandatory before their respective implementations/releases. The fixed denominator remains 34.

## Cost assumptions and falsification

This design introduces no runtime profile registry or JSON lookup on query paths and makes no speedup claim. Existing codec authentication boundaries retain fixed version comparisons; compatibility negotiation must not repeat a complete integrity walk for each query. New layouts must account for headers/descriptors, positioning, verification, page COW and reclamation, not just value bytes. Repeated parsing, unbounded accounting or a need to skip authentication invalidates the corresponding production admission.

Migration resource bounds include source/target pages and values, derived index rebuilding, allocator headroom and buffers. Representative query samples do not replace complete verification. Performance/space hypotheses remain for A3/A4/A6 and the final window; codec module runtime is not speedup evidence.
