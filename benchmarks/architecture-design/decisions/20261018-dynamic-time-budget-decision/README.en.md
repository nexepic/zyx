# Dynamic correctness time-budget decision

Approval pending: change only the full dynamic correctness case deadline from30to60minutes, for the existing three independent rounds per supported engine. Preserve10000entities, exact old/current snapshot oracles, every file/memory cap, all other30minute cases and10%/5% performance gates. Retain all actual old timeouts; never pool them with successes or repeat until pass. No amendment is applied before user approval. The648e60ff case really timed out at1800s,last CREATE8347,55750recorded operations,0operation errors,not a complete pass. A tiny cache-context experiment is rejected and its patch/raw diagnostics retained; it is not a new production requirement. Progress31/34(91.2%),request2/5,Kuzu objective unmet.

## Review of the proposed deadline change

From the repository root, run `python3 benchmarks/architecture-design/decisions/20261018-dynamic-time-budget-decision/verify-proposed-family-budget.py` to reproduce the five clock-boundary checks. It applies the patch only in memory and verifies that the real runner remains unchanged; it starts no full scenario and raises no actual deadline.

Resume inspection on 2026-10-18 found that changing the parent process wait alone leaves `ContractFamilyProgress.remaining()` at 1800 seconds, also limiting each NativeWorker call. The proposal now includes `proposed-family-budget.patch` and an explicit setting passed only to dynamic contract child processes. Five isolated clock checks cover the default 1800 seconds, proposed dynamic 3600 seconds, unchanged other-family 1800 seconds, unchanged smoke 120 seconds, and rejection of an unregistered deadline. See `proposed-family-budget-proof.json`. Neither patch has been applied to the evaluation runner; explicit approval remains pending.
