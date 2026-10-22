# Concurrency and Performance

`tests/benchmark.py` drives the real Release server over loopback TCP with
MySQL persistence. Its asynchronous peers implement the JSON protocol,
JWT authentication, message validation, and delivery ACKs. The console is tested
separately by [process integration](testing.md#real-server-and-client-processes).

## Reproduce

First create `chatroom_test`, import the schema, and set database credentials as
shown in [MySQL Tests](testing.md#mysql-tests). Then build a Release server:

```bash
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLIENT=OFF -DBUILD_TESTING=OFF -DBUILD_DATABASE_TESTS=OFF
cmake --build build-performance -j2
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance-pilot --pilot --warmup 1
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance
```

The pilot performs short checks; the final command runs the default matrix.
For an individual workload, specify its kind, connections, TCP workers, and
duration. This holds 10000 connections across the default 90-second idle timeout:

```bash
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance-long-idle --case idle 10000 4 120
```

Use Python 3 and the MySQL command-line client. There are no external Python
packages or pip installs. Run without other database tests. The harness requires
`CHATROOM_DB_NAME=chatroom_test`, creates unique records, and deletes only its
own records afterward. Seed users have unusable password hashes and locally
signed test JWTs, so setup excludes HTTP login and password-hashing cost.

Logs and `results.json` go to the selected output directory. Passwords and tokens
are not written to results. The committed
[performance-results.json](performance-results.json) contains baseline,
extended-idle, and lock-profile runs.

## Method

| Workload | Connections | Behavior |
| --- | --- | --- |
| Idle | 1000, 5000, 10000 | Authenticate, hold 45 seconds, stagger heartbeats every 15 seconds, check every peer afterward |
| Ping | 32, 256 | Send one request and wait for its response |
| Private chat | 16, 64, 256 | Fixed pairs; each producer waits for persistence, delivery, and ACK completion |
| Groups | 64, 256, 512 | Independent 64-member groups; one producer per group waits for all 63 recipients' ACKs |
| Sustained chat | 64 | Private chat for 120 seconds |

Busy cases use 128-byte ASCII text, a three-second warmup, and a 20-second
measurement window, repeated three times. Final in-flight work is drained and
included in elapsed time. The 256-peer private case compares four and eight TCP
workers. Connection limits are raised for idle runs; ordinary request limits
remain 32 per connection and 1024 globally.

This is a closed-loop load: each producer waits for its previous operation.
Latency starts before serialization and sending. Response latency ends at the
persistence response; delivery latency ends when a recipient parses the notice;
ACK latency ends after the sender's response and every recipient's ACK response.
For a group, that includes its slowest recipient. Percentiles use nearest rank.

The harness validates notification contents, database message/delivery counts
including warmup, pending ACK counts, peer failures, and server warnings.
Duplicate notifications are counted separately. CPU, RSS, descriptors, and
threads are recorded. CPU 100% means one logical CPU; server RSS excludes
kernel TCP buffers, MySQL, and generator memory.

All processes share one WSL2 host. Results include local contention and exclude
real-network conditions. Connection capacity, ping rate, and persisted-message
throughput are separate measurements; closed-loop percentiles do not predict
latency under an independent fixed arrival rate.

## Local Results: October 21, 2026

The application source was commit `2ac4dca`; no application code or database
durability settings were changed for this benchmark. The server was built with
GCC 13.3.0 in CMake Release mode. The host was WSL2 on Linux 6.6.87.2, with eight
logical CPUs from an AMD Ryzen AI 9 HX 370 and 12,249,312 KiB of visible memory.
The open-file limit was 1,048,576. Server, MySQL, and generator shared this host.

MySQL was 8.0.46, with a 128 MiB InnoDB buffer pool, binary logging enabled,
`innodb_flush_log_at_trx_commit=1`, and `sync_binlog=1`. These results include
durable commits and ACK updates. They do not represent an in-memory chat relay.

### Connections

All three initial holds lasted 45 seconds. No connection failed setup or the
final heartbeat check. Four chat workers were used throughout, and the total
server thread count remained 10, including the HTTP listener and HTTP workers.

| Authenticated connections | Setup, including JWT authentication | Server CPU | Server peak RSS | Heartbeat p99 | Server file descriptors |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1,000 | 0.398 s | 1.78% | 16.17 MiB | 0.988 ms | 1,012 |
| 5,000 | 1.566 s | 6.80% | 21.89 MiB | 0.824 ms | 5,012 |
| 10,000 | 3.567 s | 10.38% | 28.75 MiB | 6.201 ms | 10,012 |

### Sustained Throughput and Latency

Each value below is the median of three runs. Percentiles use nearest rank
within each run; the table reports the median of those per-run percentiles.
They are not percentiles pooled across all three runs. Throughput ranges expose
run-to-run variation on the shared WSL2 host.

| Workload | Connections | Chat workers | Completed operations/s, median (range) | Recipient deliveries/s |
| --- | ---: | ---: | ---: | ---: |
| Ping | 32 | 4 | 25,877 (24,041-25,908) | N/A |
| Ping | 256 | 4 | 26,810 (25,067-26,872) | N/A |
| Private chat | 16 | 4 | 310.48 (304.09-312.95) | 310.48 |
| Private chat | 64 | 4 | 364.54 (340.68-385.04) | 364.54 |
| Private chat | 256 | 4 | 335.69 (311.70-344.84) | 335.69 |
| Private chat | 256 | 8 | 596.44 (500.58-602.28) | 596.44 |
| One 64-member group | 64 | 4 | 8.56 (8.46-8.95) | 539.30 |
| Four 64-member groups | 256 | 4 | 10.69 (10.62-10.95) | 673.43 |
| Eight 64-member groups | 512 | 4 | 10.88 (10.65-11.12) | 685.44 |

A chat operation finishes only after all recipient ACK responses return. The
group columns count distinct persisted group messages and their 63 recipient
deliveries separately. Ping operations do not save messages.

| Chat workload | Delivery p50 | Delivery p95 | Delivery p99 | All ACKs complete, p99 |
| --- | ---: | ---: | ---: | ---: |
| Private, 16 peers, 4 workers | 41.778 ms | 48.110 ms | 51.942 ms | 98.340 ms |
| Private, 64 peers, 4 workers | 84.136 ms | 132.027 ms | 153.948 ms | 240.676 ms |
| Private, 256 peers, 4 workers | 359.901 ms | 546.194 ms | 600.209 ms | 1,028.643 ms |
| Private, 256 peers, 8 workers | 209.448 ms | 336.583 ms | 391.490 ms | 656.051 ms |
| One group, 4 workers | 27.985 ms | 42.807 ms | 47.539 ms | 189.639 ms |
| Four groups, 4 workers | 133.070 ms | 324.822 ms | 378.880 ms | 625.361 ms |
| Eight groups, 4 workers | 322.706 ms | 645.404 ms | 754.809 ms | 1,028.510 ms |

| Workload | Server CPU | MySQL CPU | Generator CPU | Server peak RSS |
| --- | ---: | ---: | ---: | ---: |
| Ping, 32 peers | 136.84% | 0.65% | 48.45% | 15.64 MiB |
| Private, 16 peers, 4 workers | 35.85% | 74.70% | 12.02% | 15.50 MiB |
| Private, 64 peers, 4 workers | 42.20% | 87.09% | 15.91% | 15.39 MiB |
| Private, 256 peers, 4 workers | 38.00% | 77.07% | 14.94% | 15.38 MiB |
| Private, 256 peers, 8 workers | 52.92% | 119.65% | 19.67% | 19.01 MiB |
| Eight groups, 4 workers | 34.81% | 77.66% | 10.54% | 16.01 MiB |

The generator did not exhaust its logical CPU in these runs. MySQL and generator
RSS are recorded separately in the raw results. The eight-worker server used
14 total threads, compared with 10 in the default four-worker configuration.

### Extended Checks

The 10,000-connection hold was repeated for 120.069 seconds, longer than the
configured 90-second idle timeout. Staggered heartbeats kept all 10,000 authenticated
connections responsive. Server RSS started and ended at 29.01 MiB, with a 29.01 MiB
sampled peak, 10 threads, and 10.33% mean CPU. Heartbeat p50/p95/p99 were
0.265/0.758/3.317 ms. The worst heartbeat was 256.317 ms, so the percentile should
not be read as an upper bound.

In the separate 120.127-second private-chat run, 64 peers completed 38,809 measured
messages and ACKs at 323.07 messages/s. Server RSS stayed at 15.39 MiB across the
measurement; mean server CPU was 37.69%. Delivery p99 was 201.586 ms and complete
ACK p99 was 343.967 ms. Two minutes without RSS growth is a bounded observation,
rather than a long-term leak guarantee.

Across the 31 baseline runs, database checks accounted for 152,432 saved messages
and 286,352 recipient deliveries, including warmup. All were acknowledged, with
zero content mismatches, duplicate notifications, peer errors, or server warnings.
The supplemental connection hold and two additional group runs also passed.
Benchmark-owned test users were removed afterward. A final walkthrough with the
Release server and independently built Release console passed registration,
login, messaging, groups, files, reconnect, and both graceful and forced restart.

The raw resource measurements include MySQL separately: its sampled RSS ranged
from about 458 to 550 MiB across these runs. This memory is additional to the
server RSS quoted above.

## Interpretation

The server held 10000 authenticated connections with a fixed thread count and
about 29 MiB of process memory during the 120-second check. With eight TCP
workers, private chat reached a median 596 fully persisted and acknowledged
messages/s across 256 active peers. Holding many connections and serving many
active senders are distinct capabilities; the ping rate is not chat throughput.

The results point to synchronous database work as the first chat bottleneck:
database-free ping is much faster, server CPU stays below one core during chat,
and eight workers improve the 256-peer private median by about 78%. Four-worker
throughput flattens as active peers increase, while tail latency rises.

Private chat recorded no row-lock waits; the six baseline multi-group runs
recorded 210. InnoDB log-buffer waits were zero. Two supplemental 20-second group
runs measured 20 waits totaling 98 ms and 62 waits totaling 216 ms. These totals
do not identify row-lock contention as the main throughput limit. SQL execution,
network round trips, disk synchronization, and commit scheduling were not timed
individually, so the measurements do not isolate a single database cause.

Reducing SQL round trips, such as batching recipient delivery inserts, remains
a possible future improvement. No database optimization was applied for these
results. The observed durations and local host establish a reproducible project
benchmark, not a production capacity limit or a long-term memory guarantee.
