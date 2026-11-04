# Index residual batches and current budget

The 041163ad standard Ninja binary completed ordinary tests with 10986 passes and 18 failures (11004 executed, no skips, 20 disabled). Those failures are covered by the three suites now exercised here; this does not rewrite the original full result.

77eab95f migrates fixtures to source admission, including edge copies, explicit unsupported rejection, and malformed shapes emitted without payload copies. The 600-candidate range fixture now streams bounded pages; it retains its 8 KiB materialization limit and LIMIT/result assertions.

With those fixtures, the original residual capacity calculation still failed: the budget held 7847 of 7880 available execution bytes and a reservation needed to grow 702 to 758. It sized residual pages using the original configured limit while visited candidate IDs remained charged across pages. Using the current available allowance fixes that case without increasing any budget.

The standard Ninja build completed successfully. The same-tree binary passed the three affected suites 208/208 and the broader LogicalPlan*/LogicalGraphReadExecutor* query selection 963/963. XML is retained alongside this note. This is Linux, not native Windows/macOS verification, and is not a fresh full-suite result. Native 6000-byte partial decode, Windows resource failures and ASan process RSS remain unresolved. No remote push or CI trigger was performed.
