Fix recovery and optional cache read admission

Startup recovery and scalar configuration scratch use the existing MAINT control grant. Ordinary copy reads bypass denied cache residency; explicit pins remain strict. No budget or format changes.

All 2614 tests from 121 affected modules passed, alongside 13-family full-result smoke, 4000 four-client operations, complete node/edge counts for both large inputs, and 28 campaign module tests. No full suite or formal performance acceptance. Progress remains 26/34 (76.5%), 2/5 this request.
