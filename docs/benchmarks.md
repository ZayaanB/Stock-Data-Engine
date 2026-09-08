# Benchmark methodology and results

Measured on 2026-09-03 using GCC 14.3.0 and `-O3 -DNDEBUG -march=native` on a 13th Gen Intel Core i9-13900H running Linux 7.0.0.

| Benchmark | Count | Throughput | Latency |
|---|---:|---:|---:|
| Google Benchmark add/cancel | 2,109,186 iterations | 58.19 Mops/s | 34.4 ns per add/cancel pair |
| Mixed framed file replay | 10,000,000 msgs | 3.92 Mmsg/s median | p50 229 ns, p90 353 ns, p99 545 ns, p99.9 937 ns |
| Public historical PCAP/MoldUDP64 replay | 20,288,210 msgs | 3.15 Mmsg/s median | p50 161 ns, p90 313 ns, p99 663 ns, p99.9 1,382 ns |

Replay latency uses `steady_clock` around book dispatch for every tenth message, capped at one million preallocated samples. Sorting happens after timing. Three replay runs were used; the table reports median throughput and the middle latency result at each percentile. Max latency is reported by the CLI but should not be generalized from a non-isolated desktop system.

The attempted `perf stat` run was rejected by the host (`perf_event_paranoid=4`). Run the command in the README on a host granting performance-counter access and record cycles/message, instructions/message, branch misses/message, and cache misses/message. Do not compare counter runs across different workload sizes or compiler flags.

## Historical validation

Input: Databento public `ny4-xnas-tvitch-a-20230822T133000.pcap.zst`, SHA-256 `387e1d21228fe934c2d58ed896ebfe80a7289eadee741f34f6eec846b5812204`.

- 15,777,210 PCAP/UDP/MoldUDP64 packets;
- 20,288,210 MoldUDP64 messages;
- all 20,288,210 messages decoded across all 22 ITCH 5.0 message types;
- zero sequence gaps, rewinds, malformed packets, malformed ITCH messages, Mold errors, or rejected book events;
- final AAPL quote: 176.5100 x 122 bid and 176.5200 x 754 ask.
