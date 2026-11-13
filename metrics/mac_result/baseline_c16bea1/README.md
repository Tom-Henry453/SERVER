# Baseline at commit c16bea1

Closed-loop load test of the server as it stands before any performance work.
Every later change is compared against these numbers.

Each level was run three times for 10 seconds. The table shows the run with the
median throughput, and the spread across all three.

| Connections | Throughput (req/s) | Spread of 3 runs | Mean | p50 | p99 | p99.9 | Max | Errors |
|---|---|---|---|---|---|---|---|---|
| 1 | 7,902 | 7,902 to 7,993 | 0.13 ms | 0.15 ms | 0.20 ms | 0.30 ms | 18 ms | 0 |
| 4 | 20,398 | 20,393 to 20,698 | 0.20 ms | 0.17 ms | 0.30 ms | 0.42 ms | 103 ms | 0 |
| 10 | 26,038 | 25,814 to 26,221 | 0.38 ms | 0.36 ms | 0.58 ms | 0.87 ms | 90 ms | 0 |
| 25 | 27,476 | 26,994 to 28,266 | 0.91 ms | 0.85 ms | 1.16 ms | 3.76 ms | 70 ms | 0 |
| 50 | 27,595 | 27,488 to 27,674 | 1.81 ms | 1.69 ms | 2.35 ms | 61.20 ms | 131 ms | 0 |
| 100 | 27,846 | 27,396 to 27,852 | 3.59 ms | 3.31 ms | 4.35 ms | 67.38 ms | 138 ms | 0 |
| 200 | 27,705 | 27,658 to 28,213 | 7.22 ms | 6.58 ms | 10.00 ms | 77.67 ms | 90 ms | 0 |
| 400 | 27,664 | 26,726 to 27,846 | 14.45 ms | 13.20 ms | 60.62 ms | 121.92 ms | 161 ms | 0 |
| 800 | 28,194 | 28,177 to 28,388 | 28.33 ms | 27.59 ms | 82.87 ms | 89.47 ms | 92 ms | 0 |

Latencies are measured by the client and include opening the TCP connection.

## What it shows

- Throughput levels off at about 28,000 requests per second from 25 connections
  upward. Four worker threads are saturated at that point.
- Past saturation, mean latency grows in proportion to the number of
  connections (1.8 ms at 50, 3.6 ms at 100, 7.2 ms at 200, 14.4 ms at 400,
  28.3 ms at 800). The extra clients are waiting in the queue, as Little's Law
  predicts: concurrency = throughput x latency. The `littles_law_rps` column in
  the CSV files is concurrency divided by mean latency, and matches the measured
  throughput on every row.
- No request failed in any of the 27 runs, and the server stayed up throughout.

## Conditions

| | |
|---|---|
| Machine | Apple M5, 10 cores, macOS 27.0.1 |
| Power | mains, Low Power Mode off |
| Compiler | Apple clang version 21.0.0 (clang-2100.4.1.1) |
| Build | `g++ -std=c++23 -O2 -pthread -I. SERVER.cpp` |
| Load generator | fortio 1.76.0, on the same machine, over loopback |
| Request | `GET /` (index.html, 2,543 bytes), keep-alive off |
| `kern.ipc.somaxconn` | 2048 (the macOS default of 128 causes refused connections above about 200) |
| Date | 2026-11-13 12:08:41 |

Things that affect these numbers and should not be forgotten:

- The load generator and the server share the same 10 cores.
- The server writes a log line for every connection.
- With keep-alive off, every request pays for a new TCP connection.
- On battery with Low Power Mode on, the same test gave about 17,500 requests
  per second, a third lower.

## Files

- `baseline.csv`: one row per connection level (median run).
- `all_runs.csv`: all 27 runs.
- `json/`: Fortio's raw output per run. View with
  `fortio report -data-dir metrics/mac_result/baseline_c16bea1/json`.
- `environment.txt`: machine state recorded by the script.

## Reproduce

```
sudo sysctl -w kern.ipc.somaxconn=2048
git checkout c16bea1
metrics/mac_result/run_baseline.sh
```
