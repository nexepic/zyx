### 2026-10-20 R3.2：扩展字段晋升与阈值往返

证据：`benchmarks/architecture-design/decisions/20261020-r32-durable-field-promotion/`。

- 显式生命周期仅对本次实际变更的extension token重判列资格，每个token最多64个存活槽、每批最多64个token。基于同一source pin读取并叠加整批最终替换/删除/新增，满足至少48个成员且非NULL值为同一int/double/bool类型时晋升。纯NULL字段不猜测类型；异型/复杂/大值来源保留独立字段。
- 只复制实际存放该字段的页，擦除最终成员对应的extension slot，放入共享私有页中的列记录；更新owner来源、精确extension suffix、列目录和group列计数。与其他字段变更/退休共享一次A/B发布，不重建整个group，也不创建per-owner树。字段扫描、短scalar解码、私有记录和目录镜像仍属于原1 MiB账户。
- 15个模块默认302通过；候选452通过、1个条件跳过。新增覆盖首次插入64槽稠密double字段、NULL/-0/NaN位模式、晋升时A/B完整恢复、47→48重新晋升及逆转换、两个token同批晋升、修复唯一异型extension、纯NULL等候类型，以及额度/写盘前后取消退款和重试。没有全量单测或性能基准。

通用类型/大值内容索引及引用回收、整owner/Entity Map联动、多组、native搬迁、生产接入和最终完整成本/同执行器ROW对照仍待完成。**尚无新增阶段验收，总计13/24（54.2%）；不提交、不推送。**

额度测试首轮将首次建图误设为128 KiB必然拒绝；实际可在该额度内成功。已改用已有extension目录晋升触发增量覆盖层计费，保留失败证据，最终模块回归通过。
