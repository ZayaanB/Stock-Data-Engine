# Optimization notes

## Implemented choices

1. **Pool storage instead of per-order heap allocation.** Stable contiguous storage makes add/delete constant time and the allocation-counter test observes zero allocations over 100,000 add/cancel cycles.
2. **Flat lookup instead of `std::unordered_map`.** A fixed table removes node allocation and pointer chasing. Backward-shift deletion is important: tombstones eventually make unsuccessful probes scan the whole table in high-churn workloads.
3. **Intrusive queues and indexed AVL levels.** FIFO state adds two pointers to `Order`; no list nodes are allocated. Balanced price trees avoid dense price-range scans and ordered-container node allocations.

## Measured experiments

| Experiment | Baseline | Candidate | Result |
|---|---:|---:|---:|
| Tombstone vs backward-shift deletion, 100K high-churn lookup cycles | 25,502.83 ns/cycle | 7.61 ns/cycle | backward shift 3,349.81x faster after tombstone saturation |
| Portable vs `-march=native`, five 10M-op runs | 57.18 Mops/s median | 56.60 Mops/s median | native was 1.0% slower; retain as an opt-in flag |
| No latency sampling vs 1-in-10 sampling, three 10M-message replays | 3.96 Mmsg/s median | 3.92 Mmsg/s median | sampling cost approximately 1.0% in this noisy run |

The tombstone experiment intentionally models a long-running, high-churn table. Once every slot has been used, naive tombstone lookup approaches a full-table scan; backward-shift deletion restores early termination at empty slots. `lookup_policy_benchmark` reproduces the comparison.

The current Google Benchmark mutation measurement is 58.19 M operations/s. The end-to-end framed replay median is 3.92 M messages/s because it additionally includes file IO, wire decoding, variant dispatch, a much larger 1.25M-order working set, and sampled clocks. These are different workloads, not an optimization ratio.

## Reproducible experiment protocol

For future changes, record a baseline from at least five runs, change one factor, rebuild cleanly, repeat on the same pinned CPU, then report median and dispersion. Useful next comparisons are AVL versus a dense price ladder, 50/70/85% hash load factors, and byte-at-a-time parsing versus `memcpy` plus byte-swap. Retain regressions as documented negative results instead of selecting only favorable runs.
