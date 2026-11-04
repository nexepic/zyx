# Scalar string decode construction and retention credit

The existing libc++ fresh-string upper bound remains unchanged. Canonical scalar
string decoding reserves that upper bound before construction. After successful
construction a workspace can refund the difference between that grant and the
observed retained string buffer request, including the existing supported-STL
proxy/alignment accounting. Compound values and scopes without a refund callback
retain their existing conservative grant. Values must still die before the
workspace. No allowance, source admission or sanitizer setting is changed.

Linux Clang 19.1.7, Debian libc++ 19.1.7-3+b1, standalone source compilation:
`check.cpp`, codec, Format.cpp and PerfTrace.cpp, `-O0 -g0 -stdlib=libc++`.
Same check and sources except codec-before is the file from 13bc05df:

* Before: full 16,385-byte construction grant per 8,192-byte string; 1 MiB
  workspace rejects string 64 with 1,032,255 held for 63 retained strings.
* After: 64 live strings, 524,800 bytes charged; the construction grant is still
  16,385. Upper-minus-one rejects before cursor advancement, leaving charge zero.
* Independent global C++ new/delete interception in check-allocations.cpp observes
  exactly 64 allocations, 524,800 live/peak requested bytes, no tracking overflow;
  charge and tracked live allocations return to zero after teardown. This is
  requested C++ heap memory, not malloc metadata, RSS or Apple runtime evidence.

Same-tree standard Ninja build completed successfully and subsequent invocation
reported no work. Focused suite 51/51 passes including codec inspection, session
budgets, allocator costs, native tight/ample reads and coordinated prefetch.
The broader shape/64-owner conversion suite is recorded separately when complete.
This is not the required complete coverage run or macOS/Windows CI acceptance.
Native 6,000-byte retry duplication and other CI budget/RSS failures remain open.
