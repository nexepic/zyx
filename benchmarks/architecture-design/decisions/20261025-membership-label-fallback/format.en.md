# Versioned primary-label fallback

Normal bitmap records retain their exact version-1 wire format. A group whose complete primary label union exceeds 64 tokens uses a version-2 record with the same 64-byte header, mode 1 at offset 42, zero label count and no label body. The existing header/body checksums cover this canonical encoding. Other version/mode pairs, reserved bits and mixed fallback/bitmap bodies are refused. Node live bits, database/group identity and semantic generation keep their original validation. No limit or adoption gate changes.

Construction still reads complete primary Entity/LabelSet sources, including propertyless and unlabeled nodes. The 65th distinct token switches the whole group to primary-label mode; no label prefix is published. Full integrity derives the same canonical mode and live bits from the primary stream, so checksum-valid forged fallback flags are rejected. The ordinary 64-token bitmap record is never reinterpreted as a partial label set.

An empty label predicate uses stored live bits directly. Nonempty predicates read the selected group’s live primary records and complete LabelSets under the same verified snapshot and finite source workspace, then intersect all required tokens. The existing synchronous grant covers only selected IDs. No primary-label read cache or enlarged label bitmap is added. Account denial/cancellation propagates without replay or truncation.

This is an explicit record-level extension inside experimental format 15/41; descriptor version 2 and ordinary format 5/31 and 13/39 opening rules stay unchanged. Old version-1 record decoders refuse fallback records rather than misread them. Atomic maintenance after node/label updates remains future work; writable public opening is still refused.
