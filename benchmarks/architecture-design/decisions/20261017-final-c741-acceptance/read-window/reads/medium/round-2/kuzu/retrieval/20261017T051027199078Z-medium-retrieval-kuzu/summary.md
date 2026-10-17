# 图数据库性能对比报告

## Summary

| database | workload | scale | samples | first_ms | min_ms | avg_ms | p50_ms | p95_ms | p99_ms | max_ms | ops_per_sec | status | equivalent_mode |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| kuzu | batch_node_fetch_100 | medium | 10 | 0.4699809942394495 | 0.4524649993982166 | 0.4999759985366836 | 0.47115300549194217 | 0.6019950087647885 | 0.6019950087647885 | 0.6019950087647885 | 2000.0960104620488 | ok | cypher |
| kuzu | one_hop_fetch_neighbor_ids | medium | 10 | 0.5286880186758935 | 0.5169899959582835 | 0.6135642033768818 | 0.5477750091813505 | 0.9253009920939803 | 0.9253009920939803 | 0.9253009920939803 | 1629.8212876440414 | ok | cypher |
| kuzu | one_hop_fetch_neighbor_records | medium | 10 | 0.6605719972867519 | 0.6376579985953867 | 0.8037479012273252 | 0.6747520237695426 | 1.50678597856313 | 1.50678597856313 | 1.50678597856313 | 1244.1712114868324 | ok | cypher |
| kuzu | point_edge_fetch_by_endpoints | medium | 10 | 0.4311439988669008 | 0.3966029908042401 | 0.4296106053516269 | 0.40421501034870744 | 0.5466529983095825 | 0.5466529983095825 | 0.5466529983095825 | 2327.6892784839933 | ok | cypher |
| kuzu | point_node_fetch_by_id | medium | 10 | 0.2203339827246964 | 0.21296300110407174 | 0.2631228999234736 | 0.2203339827246964 | 0.34440599847584963 | 0.34440599847584963 | 0.34440599847584963 | 3800.5053923122578 | ok | cypher |

## Limitations

- Benchmarks run through adapter-defined equivalent Cypher workloads and may not cover every engine-specific optimization.
- Results depend on local hardware, dataset scale, cache state, and runtime configuration captured in environment.json.
