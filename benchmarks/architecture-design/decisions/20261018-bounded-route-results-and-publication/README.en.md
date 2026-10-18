# Bounded authenticated route reuse and one publication collection

At most32authenticated positive route results share the existing1MiB/128-entry metadata cache and LRU. Keys include StorageIO scope,database/layout,generation/LSN,physical route root and MetadataPageId. Misses retain complete CRC/structure/visibility checks; callers still check data-page owner/generation. Every write epoch invalidates entries, future candidates are not admitted, quota denial falls back, and sequential scopes remain unchanged. No persistent format or DriverABI change.

Publication collects one complete route set for both physical bootstrap construction and logical-root identities; bootstrap plumbing adds no routed targets. No sibling or missing-route validation is skipped.

All790tests from53necessary modules pass, including11new cache cases covering namespaces,fences,write invalidation,shared bounds/LRU,actual grants,denial fallback,mixed hits and corruption/misses. No full unit suite. The initial expanded run exposed an old>1000RAW-page pressure fixture reduced to198pages by automatic compression; explicitly disable adaptive placement for that fixture, preserving its assertion and8MiB budget. Keep the original failure. All886full-result smoke checks across13families pass.

Separate copies of one mature endpoint reproduce complete values and exactly identical closed database SHA256. Logical reads37023744→35418112bytes,writes2222720both. Instrumented diagnosis may overlap builds: no duration comparison or performance acceptance. Full deadlines,read/write P50/P95,space and RSS gates remain pending under original1800s/2GiB/6GiB/64MiB and10%/5% rules.

Progress31/34 (91.2%) unchanged. Frequent full reclamation scans in large cases still require diagnosis; mechanism and unit checks do not replace acceptance.
