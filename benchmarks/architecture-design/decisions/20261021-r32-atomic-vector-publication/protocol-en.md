# Explicit native field/Vector compound publication

Scope: R3.2/R6.1 correctness prototype, experimental format32. Default format31 remains ROW. This is not production or performance admission.

## Authority and sequence

1. Stage native fields, maps or owner deletion through the existing bounded transaction. Read stores and the source pin are acquired outside the metadata writer.
2. `coordinateHybridPropertyVectors` validates same-domain Vector stores and finite options. Preparation sets a pending fence before allocating retained semantic work; failure thereafter prevents commit until rollback, including across moves.
3. Selected old values are decoded one owner at a time. Unchanged tokens need no value decoding. ROW equality is applied before BFloat16/metric planning, preserving signed-zero no-ops and strict property semantics.
4. An independent temporary native transaction stages the existing bootstrap/delta protocol against exactly the same source generation. Only its derived work is transferred. Original transaction staging guards remain active, including during callbacks.
5. Commit constructs `NativeHybridVectorPublication` on its own stack. Its constructor is private to the transaction/metadata implementation; it binds the metadata instance, exact derived-state object, admitted immutable field request and complete semantic transitions. It is not serialized or returned to callers.
6. Metadata refuses unrelated Entity/ROW/LargeValue requests, private artifacts and deferred/early planners through this proof. Derived tree IDs must belong to the Vector bootstrap/delta work. The existing typed Vector writer validates mappings, manifests and assigned identities.
7. The private field preparer authenticates schema/aliases, source owner/label scopes, old/final selected values, constraints and every expected Vector transition. Every semantic transition must be consumed. Native scalar/support/count updates remain in that same preparation.
8. Field/directory/scalar/support/count/LargeValue work and existing Vector forward/reverse/delta mapping, Raw, Manifest and retirement work share one MetadataWriteBatch. Reserved slots and assigned identities compose in the common batch. Only the outer writer publishes; reuse hints are recorded after its outcome.

Stores passed to coordination must outlive commit, as for ordinary Vector staging. Read-only adapters on writable metadata prevent allocator reconciliation during planning; they do not grant writes to a physically read-only backend.

## Failure and lifetime contract

| Boundary | Result |
| --- | --- |
| Invalid options/domain before preparation | No staged mutation or publication |
| Value/operation/quota failure during coordination | Pending fence blocks commit; rollback releases buffers and credit |
| Known cancellation before commit preparation | Active complete plan can retry after cancellation clears |
| Later commit preparation failure | Commit cannot retry a partially consumed plan |
| Constraint violation | No generation/IO-epoch change; original fields/mappings remain |
| Partial inactive Superblock write | Reopen selects old complete field/Vector state |
| Complete inactive Superblock write | Reopen selects new complete field/Vector state |
| Moves | Account, pending fence, semantic plan and typed store bindings follow the transaction |
| Old pin | Old fields and original Raw records remain readable |
| ROW signed-zero no-op | No generation, VectorId or private IO change |

## Bounds and remaining gates

Existing field limits remain64 changed cells/owners and one1MiB account. Coordination has at most128 combined semantic transitions,4096 schema definitions,128 Raw-page units,4096 configured overlay entries and128 delta-page units. These bound operations/control state rather than graph population. The existing byte account can refuse before these maxima.

PMR containers are admitted directly. Conservative credit is retained before the ordinary Vector planner's unmanaged buffers:64KiB per affected index, owner/dimension frontiers and512B per source delta entry. Actual64-owner and512-dimensional cases fit the original1MiB account and refund. This is not a process-RSS proof. A populated overlay can exceed the workspace and require maintenance; mature steady-state/production resource gates remain open.

Current affected evidence is13 candidate tests and51 default/ordinary-Vector/query controls. The prior58-module candidate receipt retains its LargeValueStore read-fixture failure. The corrected fixture, the public duplicate-key exception contract and the signed-zero production fix are tested by the current affected receipt; no fresh broad-pass count is invented. Successful compound paths also inspect full integrity.

General native topology/public writer routing remains guarded. Production space gates, full resource/maintenance acceptance, remaining R5 and R7 are unfinished. No performance timing or full repository unit suite was run. Acceptance remains14/24 (58.3%); no commit/push.
