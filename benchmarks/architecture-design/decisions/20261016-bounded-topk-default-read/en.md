Bounded Top-K on the default query path

The full medium run passed nine semantic cases, then failed Top-K: default query scratch is disabled, so Sort retained all input rows before LIMIT 100 and exceeded 64MiB. LIMIT now provides K across only cardinality-preserving non-DISTINCT projections. A charged heap retains stable input ordinals and existing NULL/type ordering. DISTINCT/SKIP remain barriers; singleton key evaluation stays deferred.

All 2628 tests in 124 selected modules passed, including full-sort denial and bounded Top-K success under the same 8MiB cap with complete parent-account refunds. Both complete medium/holdout CSV oracles checked all 100 IDs and scores. The old performance window is excluded, with no budget increase. Progress remains 26/34 (76.5%), 2/5 this request; final performance acceptance is pending.
