# A1 conservative catalog-view identity for snapshot binding

Date: 2026-10-14. Status: identity interface and consumer boundaries implemented; final selected-module results are in results/20261014-a1-schema-observation/verification.json. S1.1 remains unfinished and physical-plan caching is disabled.

## Decision and actual storage protocol

`LogicalGraphReadSnapshot::bindingSchemaIdentity()` supplies query-local binding identity; unsupported readers return nullopt by default. Owning `LogicalGraphSchemaIdentity` contains database/format domains, six opaque catalog-view tokens, an overlay view token, and logical revision. It contains no borrowed pointer and changes neither persistent codecs nor Driver ABI. Data values and graph visibility still belong to the original snapshot/fence. This identity alone is not a complete physical-plan cache key.

The first implementation incorrectly used Superblock.catalog_root. The new-property test showed that this is not the native System Catalog tree. SystemCatalog accesses names, tokens, and definitions through BTreeIndex with RSS_SYSTEM_CATALOG_INDEX_ID. Further verification showed that IndexRootLocation root_page_id/root_generation describe logical page identity and lifetime; they can remain unchanged after leaf COW. They cannot represent catalog content versions. New-name and constraint-definition assertions therefore continued to fail and were retained.

The final identity observes the pinned Superblock's index_root_directory_root and page_directory_root, three location/incarnation tokens each. The former resolves the System Catalog's logical root; the latter resolves COW versions of its logical B+Tree pages, including descendant modifications. Queries only compare opaque values; metadata pins provide their immutable provenance. There is no full catalog scan and no claim that logical-root lifetime generation is an independent schema epoch.

This view is deliberately conservative: other indexes or data pages can COW these shared directories and invalidate bindings. It provides correct identity checks, not an independent persistent schema epoch or guaranteed reuse/cache gains after data updates. The initial expectation that value-only changes must preserve identity does not fit this page-directory protocol; that expectation was replaced with database identity, validity, and old-pin checks. New-name/definition invalidation assertions remain. Cross-query caching waits for the independent epoch/capability/semantic and complete physical-descriptor contracts.

## Overlays, invalidation, and errors

Every LogicalGraphTransactionOverlay receives a nonreused process-local view token; monotonic-counter exhaustion throws. Different transactions remain distinct even at equal base and revision. Normal/vector overlay snapshots add the same owner token/revision. Successful statements change revision; failed statement restoration preserves identity. Completed transactions reject identity reads like other reads. View tokens change neither graph mutation fences nor conflict rules.

Unsupported source readers explicitly return nullopt. Invalid supplied identities throw instead of becoming optional fallback. A mutable overlay source is conservatively declined as this transaction's immutable base. The interface retains no page/reader pointer and does not replace lifetime, parameter-type, access-path capability, language/kernel semantics, or data-snapshot checks.

## Selected validation and remaining work

New real-backend cases cover independent database files, source pins, data updates, new property names, schema-definition publication, close/reopen, and independent views of two live transactions at equal revision. Normal/vector overlays agree; updates invalidate, failed statements restore, rollback rejects, and unsupported legacy fixtures return nullopt. Existing overlay modules run alongside these tests; verification JSON and raw logs contain final results. No full suite or benchmark ran.

Remaining work includes immutable binding/physical descriptors, owning dynamic batch envelopes, explicit compatibility/removal inventory, and grouped representations. This is an implemented foundation within S1.1, not a new acceptance item or completed S2 integration. Acceptance remains 4/34 (11.8%); without a percentage increase, code and bilingual records remain in the working tree.
