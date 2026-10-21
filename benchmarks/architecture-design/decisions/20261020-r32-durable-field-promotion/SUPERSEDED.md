# Withdrawn implementation / 已撤回实现

The frozen evidence in this directory records an unaccepted prototype. Its automatic extension-to-column promotion contradicts the explicit conversion contract in `../20261019-r2-next-five/r3-layout-zh.md` (lines 15 and 67). The code has been removed. Subsequent field writes retain extensions even at 48 members or after a homogeneous type change; columns may be created by an explicit ROW-to-hybrid conversion. No acceptance percentage or commit was awarded for this prototype.

本目录保留未验收原型的原始证据。后续写自动晋升偏离定案，已撤回；原始补丁、日志和哈希不改写，不代表当前行为。浮点 no-op 同时恢复 ROW 相等语义：+0 与 -0 相等，NaN 与自身不相等。
