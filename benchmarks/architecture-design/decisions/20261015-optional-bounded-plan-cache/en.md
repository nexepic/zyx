# Optional bounded plan caching

Component evidence; still22/34 (64.7%), with no stage acceptance.

Cold state is lazy and published only after a complete first insertion; failed candidates release retained capacity. Cache-boundary quota refusals fall back to uncached compilation. Source QUERY denial, bad_alloc, semantics and corruption still propagate. Direct get without a caller scope uses the cache owner for escaping clones; Native engine hits use their QUERY scope.

Managed backends lacking a shared-cache authority disable automatic caching and discard old state, avoiding private QUERY-account retention up to the64-account ceiling. A96-query compilation/release regression checks this. A mock cache-pressure test preserves prior entries, allows compilation and retries insertion after refund; it does not claim all Native reads work with zero CACHE headroom.

All694 selected tests and871 isolated cases pass, including32 crash/physical-readonly/recovery cycles. Short fault paths are exhaustive and long paths sample at most96 allocation points. Counts are in proof.json. Dynamic containers and other ownership work remain open. No full suite, performance campaign, RSS/Kuzu benefit or macOS/Windows runtime claim.
