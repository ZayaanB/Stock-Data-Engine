# High-Performance NASDAQ ITCH Market Data & Order Book Engine

A high-performance C++23 market data engine that decodes NASDAQ TotalView-ITCH messages and reconstructs a full-depth electronic limit order book. The system is designed around preallocated memory and cache-efficient data structures to minimize processing latency and sustain millions of market updates per second.

## Measured performance

Release build (`-O3 -DNDEBUG -march=native`), GCC 14.3.0, 13th Gen Intel Core i9-13900H, Linux 7.0.0. Measurements from 2026-09-03; results are machine- and workload-specific.

| Workload | Result |
|---|---:|
| Google Benchmark add/cancel | 58.19 M operations/s |
| Framed mixed ITCH replay, 10M messages | 3.92 M messages/s (three-run median) |
| Replay latency p50 / p90 | 229 ns / 353 ns |
| Replay latency p99 / p99.9 | 545 ns / 937 ns |

Latency sampled every tenth decoded message (1M samples). The replay ended with 1,250,000 active orders, zero rejected book events, and zero malformed frames. CPU frequency scaling was enabled, so these results are indicative rather than publication-grade. Hardware counters could not be collected in this container because `perf_event_paranoid=4`; no fabricated `perf` numbers are reported.

## Project status

The core MVP is implemented and tested: recorded-file replay, all required decoders, lifecycle reconstruction, full depth, fixed-capacity memory, differential validation, benchmarks, documentation, and CI. Two PRD acceptance items require an external environment and are deliberately not claimed as complete:

- replay against a licensed or legally distributable historical NASDAQ ITCH sample;
- hardware-counter results from a Linux host that permits `perf stat`.

MoldUDP64/PCAP input and the SPSC reader pipeline are stretch milestones, not dependencies of the core engine.

The detailed requirement-by-requirement status is in [docs/audit.md](docs/audit.md).

## Architecture

```text
binary ITCH file -> 1 MiB buffered reader -> framed decoder -> typed message
                                                               |
                                                               v
fixed order pool <-> flat ID lookup <-> intrusive FIFO price levels
                                           |
                                  preallocated AVL trees
                                           |
                                depth and best bid / ask
```

All capacity-owning vectors allocate in the `OrderBook` constructor. The steady-state path uses a free list for orders and levels, an open-addressed hash table with backward-shift deletion for order IDs, and AVL trees keyed by price. Orders are the FIFO nodes themselves. No `new`, `delete`, locks, or container growth occurs during book mutation.

## Supported ITCH 5.0 messages

System Event (`S`), Stock Directory (`R`), Add Order (`A`), Add Order with MPID (`F`), Order Executed (`E`), Order Executed with Price (`C`), Order Cancel (`X`), Order Delete (`D`), Order Replace (`U`), Trade (`P`), and Cross Trade (`Q`). The decoder validates exact wire lengths, converts big-endian integer fields explicitly, rejects invalid sides, and safely carries partial frames between reads.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DITCH_NATIVE_ARCH=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The offline test executable has no downloaded dependencies. To fetch the pinned GoogleTest and Google Benchmark releases and build their targets, add `-DITCH_FETCH_DEPS=ON`. Tests cover every supported parser type, malformed and fragmented frames, FIFO behavior, lifecycle operations, depth, level removal, pool exhaustion, a fixed-seed 20K-event differential run, and an instrumented 100K-cycle proof of zero hot-path allocations. GitHub Actions runs Debug/ASan/UBSan and Release configurations.

## Generate and replay a feed

```bash
./build/generate_feed /tmp/itch.bin 10000000 mixed
./build/itch_order_book \
  --input /tmp/itch.bin --symbol AAPL \
  --max-orders 1500000 --max-levels 1000 \
  --sample-every 10 --latency-samples 1000000
```

`generate_feed` supports `mixed` and `sequential` workloads. The reader expects the standard recorded-file format: a two-byte big-endian message length followed by an ITCH message. `--symbol` is required because each CLI `OrderBook` instance represents one instrument; lifecycle events for filtered-out adds are ignored as unknown IDs.

## Benchmark and profile

```bash
./build/book_benchmark 10000000
./build/lookup_policy_benchmark 100000
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  ./build/book_benchmark 10000000
```

For stable numbers: use a release build, pin to an isolated performance core (`taskset`), set the CPU governor appropriately, warm the binary, run multiple repetitions, and report the median plus compiler/CPU/kernel details. See [docs/benchmarks.md](docs/benchmarks.md) and [docs/optimizations.md](docs/optimizations.md).

## Design decisions

- Correctness precedes tuning: the optimized implementation is compared against an STL reference model with deterministic random traffic.
- Capacity is explicit. Pool or level exhaustion returns an error rather than allocating or corrupting state.
- Price levels are balanced trees, giving O(log L) insert/remove and O(log L) best-price traversal without scanning empty price ranges.
- ITCH prices remain integer fixed-point values (four decimal places), avoiding floating-point rounding.
- Book mutation stays single-threaded. File input is buffered, but networking and MoldUDP64 are intentionally outside the core MVP.

## Resume-ready summary

Built a C++23 NASDAQ ITCH market-data engine with allocation-free order processing, fixed-capacity hash lookup, intrusive FIFO price levels, and AVL-indexed depth. On the documented synthetic workload it replayed 10M framed messages at a three-run median 3.92M messages/s with 545 ns p99 sampled processing latency; measurements are reproducible with the commands above.
