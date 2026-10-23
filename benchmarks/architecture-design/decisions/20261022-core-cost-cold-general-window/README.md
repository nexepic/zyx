# Multiple-field and exact-count cold windows

Accepted progress remains **90%**, P7 open. No timing benchmark, full unit suite, commit or push.

Original point/count readers execute in a synchronous stack lease over one decoded body per ordered same-reference run of at most64owners. This adds no owning context cache, persisted format or allowance. Actual EntityMap/no competing ROW/old selectors preflight selected owners; hot-only/no-count reads do not open cold blocks. Original output rectangles/counts lend subranges and sink ordinals translate back to request positions, with no result-array copy. Fixed-size accounted membership boundary survives nested reuse of the existing cache. After owned decoding, the physical page pin is released before hot/manifest reads. Exceptions restore parent borrowing and potentially observable output/count writes cannot trigger quota replay.

Both experimental membership configurations pass **365 related module tests**, default **119**. Four new tests prove one decode for mixed hot/cold/NULL/missing projection plus exact counts, one decode for real-LargeValue count-only requests, quota failure after output followed by a fresh point decode proving restored lease, and hot-only owning reads despite corrupt cold bytes. Existing selected, ownership, query/conversion/mutation, retirement and journal modules remain green.

This is private-candidate component evidence, not real public mode3 conversion/query/lifecycle or latency acceptance. Full collection API sharing, public conversion and mutation/reclamation/old-pin/retirement lifecycle, mature whole allocation/staged migration bootstrap and final P7 matrix remain pending. Normal mode3 publication and mutation remain closed.
