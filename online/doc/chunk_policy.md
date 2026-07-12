# Provisional chunk policy evidence

Six existing binary TPC-C traces were streamed through the exact packed-layout
estimator. They contain 8,503 transactions and 4,497,236 events with
858,808,658 packed used bytes in aggregate.

| Ordinary chunk | Reserved / used | Tail slack |
|---:|---:|---:|
| 1 KiB | 1.194300 | 166,866,630 B |
| 2 KiB | 1.100828 | 86,592,198 B |
| 4 KiB | 1.059578 | 51,165,894 B |
| 8 KiB | 1.098873 | 84,912,838 B |
| 16 KiB | 1.192086 | 164,965,062 B |
| 32 KiB | 1.390665 | 335,506,118 B |
| 64 KiB | 1.883095 | 758,409,926 B |

The provisional ordinary chunk size is therefore 4 KiB. It has the lowest
aggregate reserved-byte overhead in this corpus while avoiding the larger
segment counts of 1-2 KiB chunks. The size remains configurable.

Raw per-trace distributions are in `online/results/layout`. These committed
offline traces cannot measure peak capture bytes, savepoint rollback slack,
retry attempts, or live allocation latency. The default must be re-evaluated
with live TiDB capture before it is treated as final.

Reproduce one report with:

```sh
python3 online/tools/sample_trace_layout.py \
  bin_traces/tpcc/4s-900t-729344e-tpcc-optimistic-1w-1th-1s.trace \
  --output online/results/layout/tpcc-1w-1th-1s.json
```
