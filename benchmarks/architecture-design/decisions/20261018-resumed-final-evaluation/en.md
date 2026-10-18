# Resume final evaluation and correct read-tail supplement selection

Tasks: S2.5, S3.5, S6.2. In progress, with no new acceptance; Optimization.MDX remains the progress authority.

Remote `work` was synchronized to `4ff7d0ef`. Both current Release binaries exactly match the SHA-256 identities frozen at `648e60ff`. The original control core was rebuilt from `d09fc467`, using the same query-driver source as the current build. Rebuilt and historical binary identities and samples remain separate. Production database code was not changed.

All twelve medium/holdout CSV hashes match the original registration. Both full inputs pass six exact counts in both actual native consumers; physical readonly checks leave their hashes unchanged. Consumers clone identical files under the existing shared-input protocol. The old holdout importer actually fails with a slotted replacement capacity error; redundant old medium preparation was cancelled. Current medium continuation actually reaches the original 2 GiB limit, with its failure preserved; the full input subsequently completes using the registered 5000-row preparation granularity. Preparation output is not accepted performance sampling or an import performance pass.

The full point correctness matrix passes all three independent processes for each of three engines, including 1000 random hits and 100 misses per process. These untimed correctness runs may overlap preparation and are not performance samples. The current full wide/large-value family does not complete within its original 1800-second deadline and times out in a property update. Its failure and partial trace are retained, without unchanged-source retries for success.

The old read supplement selector considered P50 only, omitting the existing section 6.1 requirement for near-threshold protection tails or unstable P95. `runner/acceptance_sampling.py` implements those conditions; seven new tests and all 43 related campaign module tests pass. The correction is registered before any supplement in `read-tail-policy-correction.json`. Original P50/P95 gates, at most two independent hundred-sample processes per source/workload, 1800 seconds per process, and 3600 seconds per original profile/source remain unchanged. This correction accepts no delivery automatically.

The formal read/write campaign continues. See `proof.json` and `raw/` for identities, reproducible scripts and raw-record digests. Unfinished correctness, performance and resource gates remain unfinished; Kuzu superiority is not claimed. The proposed dynamic 3600-second amendment is unapplied; other deadlines are unchanged. No full unit suite was run.

The write supplement selector now applies the same near-P95 protection rule, registered before the write campaign in `write-tail-policy-correction.json`. All 45 base read processes completed with 2010 valid timed samples and full-result checks. Supplements, writes and gate review remain pending; no new acceptance.

Final campaign status: the full wide-property failure requires production repair. Remaining read supplements and the unstarted write campaign were cancelled. All 45 completed base read processes and two complete supplements remain preserved; cancelled slots are not successes. Diagnosis and new-source validation continue, with no new acceptance.
