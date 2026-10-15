# Transaction wrapper and issued-ID allocation ownership

Component implementation only: acceptance remains 22/34 (64.7%).

Native transaction wrapper allocations and retained Node/Edge issued-ID vectors share one finite WRITE-class account. Ordinary object-prefix ownership retains the allocator through polymorphic destruction, constructor failure and closed objects that outlive the backend. Read-only transaction wrapper metadata uses this accounting class too; it grants no disk-write permission. No full write quota is prepaid for an empty transaction.

ID-vector candidate copying and exact range growth admit allocator requests before allocation. Copies explicitly retain their source domain. A denied later statement preserves prior staged IDs and remains retryable; abandoned durable leases are not reused. Terminal cleanup destroys ID buffers and refunds their charge, while a retained closed wrapper keeps its own body charge until destruction. This direct parent allocator is limited to wrapper/ID operations outside publication gates; other staging and COW still need prepaid workspaces.

Validation: 159 selected native transaction/patch/rebase/write-surface/isolation/resource/scratch tests plus one new closed-domain lifetime test pass (160 distinct tests). Four isolated OOM/terminal-retention cases pass; each closed object now retains 1,640 requested-allocation bytes, including its allocation controller and ownership prefix. Two expensive native sort integration cases are excluded; no full suite or performance campaign ran.

Outstanding scope includes reservation-range and outstanding-lease metadata ownership, mutation/property/undo/overlay/private-page working buffers, commit planning and maintenance/recovery, plus partitionable operator spill and full fault/long-run acceptance. This is not S5.3/S5.4/S5.5/S6.1/S6.3 acceptance.
