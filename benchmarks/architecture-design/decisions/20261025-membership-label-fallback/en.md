# General membership fallback beyond 64 labels

S2 remains unaccepted and progress stays 15%. Complete primary label unions exceeding 64 use a canonical versioned primary-label marker, with the original live bitmap and no partial label body. Ordinary version-1 records remain unchanged. See format.en.md for encoding, validation and compatibility.

Nonempty predicates intersect complete primary LabelSets under the verified snapshot and finite source grant. Unlabelled scans use stored live bits without label rereads. No larger bitmap or cache is added. Integrity rejects checksum-valid forged fallback modes. Cancellation and budget rejection never truncate or replay a prefix.

67 isolated, 17 ordinary and 17 compatible module cases pass, including 65 labels on one node, cross-node label unions, old pins, real construction/integrity/read-only opening/query consumption, duplicate/disjoint predicates and forged-mode refusal. Exact results, recipes and source/core hashes are attached. No query benchmark, full unit suite, commit or push. Atomic node/label maintenance, recovery and real-dataset space/resource/performance acceptance remain incomplete; public writable opening remains refused.
