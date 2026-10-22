# R7.3 final archive and delivery scope

All24optimization.mdxdeliverables complete:24/24(100%). This does not mean every workload beats Kuzu or every pooled tail stays within5%. The Kuzu objective is unmet;the registered cross-process rejection,individual distributions,spikes and limitations remain distinct.

| medium workload | before ZYX P50 ms | final ZYX P50 ms | Kuzu0.11.3 P50 ms |
|---|---:|---:|---:|
| explicit read-only country filter | 207.175 | 128.446 | 1.450 |
| explicit read-only actual grouping | 245.796 | 129.259 | 2.443 |
|1000-edge batch|4661.037|498.659|5.043|
|fresh full import|timeout,no sample|277007.266|911.858|

See`../20261022-r7-final-admission/performance.csv`/`proof.json`for all inputs,cases,process P50/P95 and pooled values. Actual grouping remains~53times slower,imports~269–304times;C++/Python APIs,schema and1000-row commits/COPY differ. R3saves20%live allocation,not20%file length. Final SET pooledP50/P95remain+5.9%/+13.4%;all prior failures and post-hoc envelope uncertainty stay explicit,without a universal per-case P95guarantee.

Final selected modules pass365candidate/24default cases and8untimed preflight groups. No full suite ran. Earlier559/272unique executed final passing states,each with one existing disabled case,overlap and must not be added. Linux only;independent C0–C7/platform/coverage gates are not closed. The general8MiB integrity denial is not a pass;complete-scale graph/index audits retain their narrower scope. Sustained40updates yield17pinned/20drained timed samples and a last-seven-file plateau,not infinite steady state.

raw-evidence.tar.xz/raw-manifest.json archive every formal/repair/control typed observation,error,sample,source snapshot,build identity and module log;archive-verification.json checks every member size/SHA256. Earlier failed candidates stay separate and reused import outcomes are not new processes. Import has one sample/process;its CSV P95describes the maximum of three imports,not warm-query precision.

22primary CSV/database/cohort input files retain their original SHA. Large/skew initially recorded construction/semantics only;final current SHA and pre-window mtime are supplemental,not retroactive registered byte hashes(see fixture-provenance.json). Workers cloned shared sources to fresh private targets. Successful private DBs were cleaned by their original harness;10failed/preflight DBs remain locally for forensics. Large DB/archive/object binaries stay out of Git,with library relocation hashes retained. No raw measurement failure is deleted;unaccepted intermediate prose drafts are excluded from admission evidence.

Bilingual optimization,capability and format docs are synchronized. Default31ROW remains;33cohorts are explicit/cost-gated and old6/32reject,without automatic conversion. Retain canonical key-only plus bounded primary reuse,without covering/segmented production adoption,new backends,spill algorithms or raised caps. Production code is frozen;R7.3only verifies documentation/archive and does not repeat completed tests/benchmarks.
