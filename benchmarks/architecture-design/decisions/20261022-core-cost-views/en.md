# P3 immutable metadata views accepted

Progress 50%. All 460 affected module tests pass; no full suite or formal performance matrix.

Hot Catalog, radix, bootstrap and Allocator bitmap reads retain immutable views and resolve selected paths. Writers retain private COW images. Immutable route nodes no longer repeat whole-node codec validation for every child lookup. Type, level, visibility, leaf-route, I/O epoch, physical generation and existing resource checks remain.

One medium filter and real-group diagnostic each reduces fixed object-copy counters from 139447080/139613280 to 925056/918288 bytes (99.34%). Remaining copies are small route values. Intermediate per-type counters identified the remaining 57 MB radix/29 MB bitmap copies and the final Allocator view path removes them. These are sizeof counters, not measured DRAM traffic. Candidate diagnostics use Debug code and cannot establish latency or Kuzu claims.

The existing 128-entry/1 MiB cache and allocation owners remain; no preload or duplicate authoritative mapping. Module coverage includes zero cache, view survival, invalidation, old pins, reopen and corruption. P4/P5 remain pending.
