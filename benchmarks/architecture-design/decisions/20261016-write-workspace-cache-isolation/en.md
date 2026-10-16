# Write workspace isolation under cache pressure (revalidation pending)

Final medium preparation stopped on admission before producing query samples. Preparation in 1,000-row transactions still failed; the actual throw stack identifies B+Tree validation during write commit using CACHE. A new exhausted-CACHE negative control fails on the prior source. Neither raising limits nor batching alone fixes the attribution error.

Admit at most 32MiB of prepaid private COW workspace before publication locking, within existing WRITE/database/process caps. Private metadata decode/cached-page copies preserve an explicit caller allocation scope; cache publication copies remain CACHE-owned. S5.3/S6.1 are reopened until fresh affected regression and source freeze. Earlier acceptance remains historical evidence. Formal sampling is paused; failed preparation is retained and excluded from successful timing distributions.
