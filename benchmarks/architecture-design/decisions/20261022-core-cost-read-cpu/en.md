# Independent P0 read CPU attribution

The frozen engine archive matches P0. A Linux/x86 diagnostic observer samples interrupted PCs with a thread CPU timer only inside query windows. Each filter/group process executes 12 checked queries, paired with the same observer without sampling. There are 385/371 independent PCs; overrun-weighted ticks are not independent samples. Source/binary/archive hashes are frozen in final-observer-freeze.json. Recreate frozen headers using git archive 5500a068 (include, apps/compare_benchmark, lib/inputxx/include) at /workspace/zyx-p0-profile-source before the recorded build.

Unprofiled total wall: 1.495/1.477 s; user CPU: 1.435/1.380 s; kernel CPU: 0.060/0.097 s. Estimated off-CPU is below 0.1%. Warm reads are CPU dominated. Sampled total CPU increases roughly 3.2%/1.4% over controls; no diagnostic latency enters formal results.

Exclusive leaf-PC buckets show decoding/record checks 13.4%/12.8%, checksum 9.7%/12.4%, metadata/cache validation 10.0%/7.9%, clock/runtime 8.8%/9.1%, entity record cache 5.3%/5.2%. All remaining buckets/functions are retained, including about 28% unattributed/internal libc. Stripped libc is not assigned a misleading nearest-exported function name; only verified host IFUNC regions receive coarse labels. Without stacks, memcpy/memcmp cannot be assigned to an upstream caller. Nested timers are not summed.

Positioning, decoding, objects and repeated identity checks jointly dominate; filter/group arithmetic is not the main isolated cost. Continue the planned candidate/direct-batch work. gprofng attempts with timer-loss warnings are rejected and recorded. Exploratory PCs from superseded observers are excluded. No formal benchmark matrix was rerun.
