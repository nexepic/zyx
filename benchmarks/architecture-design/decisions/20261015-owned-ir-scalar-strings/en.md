# Admission for retained IR scalar strings

Component evidence; still22/34 (64.7%), with no stage acceptance.

Budget resources expose explicit unmanaged-allocation admission. Escaping reservations retain their owner, preserve prior credit on denied growth, and reject reuse of moved reservations. They do not allocate dummy PMR backing storage. Materialized and prepaid resources implement the authority; legacy resources explicitly do not.

OwnedStdString admits the audited request upper bound before standard-string construction and refunds after destruction. Existing const std::string references remain compatible. Copies select the active owner; moves preserve the source owner and prepay MSVC's emptied-source proxy. Replacement is transactional and heap-changing base methods are hidden.44 retained scalar fields across17 operator headers and their by-value parameters are integrated. Arrays, maps, public item fields and expressions remain open.

Container models now live in utils with compatible query aliases. Property decoding and retained strings share the fresh-string bound. Parent QUERY pressure leaving no output row slot now retains its admission-denial type, preserving the strict existing regression assertion.

All628 selected module tests and105 isolated cases pass. These include96 sampled cache-miss allocation points, terminal/codec/cancellation checks and12 actual-string-allocation shapes covering copies, moves, source reuse and replacement. No uncovered allocation request or retained storage/credit is observed. A rejecting upstream also proves reservations make no dummy allocation. Long fault paths are sampled.

Runtime evidence is Linux/clang19/libstdc++14; prior frozen STL source audits do not establish macOS/Windows execution. No performance campaign, RSS result or Kuzu improvement is claimed.
