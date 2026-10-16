# Final performance window registration (in progress, not accepted)

After S5.3/S6.1 implementation/source acceptance, run one final window with rotated control/current/Kuzu order, three independent process rounds, three warmups, ten samples, a 10% primary gate, 5% protection and the registered 30-minute stopping rule. Training is medium/seed 42; holdout/seed 314159 is not used for tuning. Real Kuzu 0.11.3 uses an explicit 64MiB pool and one query thread.

Every measured process has one allowed CPU, a 6GiB address-space cap and a 2GiB per-file cap. The per-file limit is stricter than the production 1TiB maximum and identical for all engines. Kuzu explicitly uses a 2GiB virtual database mapping to fit the address-space cap. Address space is not container/RSS accounting; native charged class limits and actual RSS are reported separately. Query materialization limits are not raised. Default synchronous durability remains enabled; OS cache is warm, not claimed cold.

The old byte-bound message and exact coordinated admission-denial message represent the same expected quota outcome. Unit tests reject unrelated errors and unexpected success. Both native versions pass all 13 semantic families over 916 smoke operations; real Kuzu equivalents also pass. Formal performance, attribution and steady-state collection remain incomplete: no S0.3/S0.4/S5.5 acceptance or Kuzu-superiority claim follows from registration.

These are reproducible Linux-workspace evidence scripts, with absolute paths identifying frozen artifacts. Native benchmark query timing, contract API/IPC/full-observation timing and intrusive profiling remain separate distributions.
