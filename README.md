# High-Performance NASDAQ ITCH Market Data & Order Book Engine

[![CI](https://github.com/ZayaanB/Stock-Data-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/ZayaanB/Stock-Data-Engine/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A high-performance C++23 market data engine that decodes NASDAQ TotalView-ITCH messages and reconstructs a full-depth electronic limit order book. The system is designed around preallocated memory and cache-efficient data structures to minimize processing latency and sustain millions of market updates per second.

## Measured performance

Release build (`-O3 -DNDEBUG -march=native`), GCC 14.3.0, 13th Gen Intel Core i9-13900H, Linux 7.0.0. Measurements from 2026-09-03 and 2026-09-04; results are machine- and workload-specific.

| Workload | Result |
|---|---:|
| Google Benchmark add/cancel | 58.19 M operations/s |
| Framed mixed ITCH replay, 10M messages | 3.92 M messages/s (three-run median) |
| Historical PCAP/MoldUDP64 replay, 20.29M messages | 3.15 M messages/s (three-run median) |
| Replay latency p50 / p90 | 229 ns / 353 ns |
| Replay latency p99 / p99.9 | 545 ns / 937 ns |

Latency sampled every tenth decoded message (1M samples). The replay ended with 1,250,000 active orders, zero rejected book events, and zero malformed frames. CPU frequency scaling was enabled, so these results are indicative rather than publication-grade. Hardware counters could not be collected in this container because `perf_event_paranoid=4`; no fabricated `perf` numbers are reported.

## Project status

The full parser, order-book, recorded-file, PCAP/MoldUDP64, sequence-validation, and SPSC pipeline scope is implemented and tested. One PRD acceptance item remains blocked by this host and is deliberately not claimed as complete:

- hardware-counter results from a Linux host that permits `perf stat`.

Historical validation used the public [Databento NASDAQ TotalView-ITCH sample](https://sample-pcaps-dl.databento.com/xnas/20230822/ny4-xnas-tvitch-a-20230822T133000.pcap.zst). Its 20,288,210 MoldUDP64 messages replay with zero sequence gaps, malformed packets, decoder errors, or rejected book events.

The detailed requirement-by-requirement status is in [docs/audit.md](docs/audit.md).

## Architecture

```text
binary ITCH file -> buffered reader -> framed decoder ---------+
PCAP -> Ethernet/VLAN -> IPv4/UDP -> MoldUDP64 + sequencing ----+-> typed message
                                                                |
                              optional fixed SPSC queue --------+
                                                                v
fixed order pool <-> flat ID lookup <-> intrusive FIFO price levels
                                           |
                                  preallocated AVL trees
                                           |
                                depth and best bid / ask
```

All capacity-owning vectors allocate in the `OrderBook` constructor. The steady-state path uses a free list for orders and levels, an open-addressed hash table with backward-shift deletion for order IDs, and AVL trees keyed by price. Orders are the FIFO nodes themselves. No `new`, `delete`, locks, or container growth occurs during book mutation.

## Supported ITCH 5.0 messages

All 22 TotalView-ITCH 5.0 message types are decoded: System Event (`S`), Stock Directory (`R`), Stock Trading Action (`H`), Reg SHO (`Y`), Market Participant Position (`L`), MWCB levels/status (`V`/`W`), IPO Quoting Period (`K`), LULD Auction Collar (`J`), Operational Halt (`h`), Add Order (`A`/`F`), Execute (`E`/`C`), Cancel (`X`), Delete (`D`), Replace (`U`), Trade (`P`), Cross Trade (`Q`), Broken Trade (`B`), NOII (`I`), and Retail Price Improvement (`N`). The decoder validates exact wire lengths, converts big-endian integer fields explicitly, rejects invalid sides, and safely carries partial frames between reads.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DITCH_NATIVE_ARCH=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The offline test executable has no downloaded dependencies. To fetch the pinned GoogleTest and Google Benchmark releases and build their targets, add `-DITCH_FETCH_DEPS=ON`. Tests cover every supported parser type, malformed and fragmented frames, FIFO behavior, lifecycle operations, depth, level removal, pool exhaustion, a fixed-seed 20K-event differential run, and an instrumented 100K-cycle proof of zero hot-path allocations. GitHub Actions runs Release, Debug/ASan/UBSan, and ThreadSanitizer configurations.

## Install and consume as a library

```bash
cmake --install build --prefix /tmp/itch-order-book
```

Downstream CMake projects can then use the exported target:

```cmake
find_package(itch_order_book 1 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE ItchOrderBook::Engine)
```

Point CMake at a non-system installation with
`-DCMAKE_PREFIX_PATH=/tmp/itch-order-book`. The install also includes the
`itch_order_book`, `generate_feed`, and benchmark executables.

## Generate and replay a feed

```bash
./build/generate_feed /tmp/itch.bin 10000000 mixed
./build/itch_order_book \
  --input /tmp/itch.bin --symbol AAPL \
  --max-orders 1500000 --max-levels 1000 \
  --sample-every 10 --latency-samples 1000000

# Optional two-thread replay through the fixed SPSC queue
./build/itch_order_book --input /tmp/itch.bin --symbol AAPL --threaded

# Historical Ethernet/IPv4/UDP/MoldUDP64 PCAP replay
zstd -d sample.pcap.zst -o sample.pcap
./build/itch_order_book --input sample.pcap --format pcap --symbol AAPL
```

`generate_feed` supports `mixed` and `sequential` workloads. The reader expects the standard recorded-file format: a two-byte big-endian message length followed by an ITCH message. `--symbol` is required because each CLI `OrderBook` instance represents one instrument; lifecycle events for filtered-out adds are ignored as unknown IDs.

The PCAP reader supports classic little- or big-endian PCAP files containing Ethernet (including stacked VLAN tags) or raw IPv4, UDP, and MoldUDP64. It validates packet boundaries before dispatch and reports sequence gaps, rewinds, heartbeats, end-of-session packets, unsupported ITCH messages, and malformed transport data.

## Benchmark and profile

```bash
./build/book_benchmark 10000000
./build/lookup_policy_benchmark 100000
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  ./build/book_benchmark 10000000
./tools/perf_stat.sh 10000000 ./build/book_benchmark 10000000
```

For stable numbers: use a release build, pin to an isolated performance core (`taskset`), set the CPU governor appropriately, warm the binary, run multiple repetitions, and report the median plus compiler/CPU/kernel details. See [docs/benchmarks.md](docs/benchmarks.md) and [docs/optimizations.md](docs/optimizations.md).

## Design decisions

- Correctness precedes tuning: the optimized implementation is compared against an STL reference model with deterministic random traffic.
- Capacity is explicit. Pool or level exhaustion returns an error rather than allocating or corrupting state.
- Price levels are balanced trees, giving O(log L) insert/remove and O(log L) best-price traversal without scanning empty price ranges.
- ITCH prices remain integer fixed-point values (four decimal places), avoiding floating-point rounding.
- Book mutation stays single-threaded. The optional reader thread communicates through a preallocated, lock-free SPSC ring with acquire/release publication.

## Resume-ready summary

Built a C++23 NASDAQ ITCH market-data engine with allocation-free order processing, fixed-capacity hash lookup, intrusive FIFO price levels, AVL-indexed depth, MoldUDP64 sequencing, PCAP ingestion, and a lock-free SPSC pipeline. Replayed a public 20.29M-message historical capture with zero gaps or rejected events at a three-run median 3.15M messages/s; measurements are reproducible with the commands above.
