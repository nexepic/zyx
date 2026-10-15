# Private source name admission

Component implementation only; progress remains 22/34 (64.7%). 238 related module tests and 105 isolated cases pass. Long source-read fault paths sample at most 96 allocation points.

Native reverse-name lookup admits fresh standard strings and forward keys through the active source decode scope. Cached PMR names admit their standard-string copies before allocation. Existing explicit callbacks retain sole authority. Node-label and property-key resolvers transfer existing strings instead of cloning them again.

Temporary duplicate-detection metadata follows the explicitly owned NativeReadSession resource when no allocation scope overrides it. Tests cover cached-copy quota rejection without changing the cache, retry, and absence of duplicate implicit charging under explicit admission.

String bounds follow the existing supported STL source audits and measure allocation requests, not RSS. This is Linux runtime evidence only. Other directory workspaces and compiler/IR remain outstanding. No new performance campaign was run.
