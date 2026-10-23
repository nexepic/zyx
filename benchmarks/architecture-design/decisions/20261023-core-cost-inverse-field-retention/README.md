# Inverse field-page retention

Progress90%, P7 open. No full suite, benchmark, commit or push.

The inverse coordinator authenticates source-page ownership and removes only selected slots in private COW images. Duplicate/stale slot locators fail. Only empty logical pages enter the erased-location set; retained pages keep their logical PageId and other groups’ slot generations, with a fresh hybrid owner header in the original atomic publication. Old physical routes remain protected by the original snapshot/retirement protocol. Retained hybrid images use their own bounded packing cursor and preserve the native frame kind.

502 related module cases pass in both experimental membership variants,154 in default. Existing inverse fault/cancellation/quota/reopen/pin coverage remains passing. New public test converts eight groups, inverses only group1, verifies all other group authorities, old reads and integrity. A separate private shared PageImage test removes one group while preserving another’s slots/source bytes/reopen and rejects duplicate deletion.

Evidence limit: the initial new test incorrectly assumed the current converter shared logical pages across groups. Actual locator enumeration disproved this; its original failed report is retained. The corrected test reports actual shared-page count, currently0, and does NOT claim to exercise shared-frame publication. Private image coverage is not public converted-layout acceptance. Mode3 public inverse/write guards remain closed. Actual cold dispatch, global demotion/owner/delete/inverse installation, whole-space bootstrap, primary query costs and final P7 remain open.
