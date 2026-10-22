# Concurrency and Performance

`tests/benchmark.py` measures the real Release server over loopback TCP with
MySQL persistence. Its asynchronous Python peers implement the existing JSON
protocol, JWT authentication, message validation, and delivery ACKs. The console
client remains covered by the functional integration walkthrough.

## Reproduce

Build an optimized server and use the disposable test database described in
[Database](database.md#integration-tests):

```bash
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_CLIENT=OFF -DBUILD_TESTING=OFF
cmake --build build-performance -j2
read -r -s -p 'Database password: ' CHATROOM_DB_PASSWORD
printf '\n'
export CHATROOM_DB_PASSWORD
export CHATROOM_DB_NAME=chatroom_test
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance --pilot --warmup 1
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance
```

To repeat an individual workload, supply its kind, connection count, TCP worker
count, and duration. For example, hold 10,000 connections across the default
90-second idle timeout:

```bash
python3 tests/benchmark.py --server build-performance/server/chatroom_server \
  --output /tmp/chatroom-performance-long-idle --case idle 10000 4 120
```

The script uses Python 3 and the MySQL command-line client, without extra Python
packages. Run it without other database tests. It inserts uniquely named users
and deletes only its own records on completion. Setup uses unusable placeholder
password hashes and locally signed, valid test JWTs so password hashing does not
dominate connection setup. HTTP registration and login are outside this benchmark.
Tokens and database passwords are never written to the result files.

Raw server logs and `results.json` are written to the output directory. Large
logs belong outside the repository. The committed result snapshot accompanies
the analysis below: [performance-results.json](performance-results.json).

## Workloads and Measurements

- Authenticated idle connections: 1,000, 5,000, and 10,000 connections, held for
  45 seconds each, with staggered heartbeats every 15 seconds. Every connection
  is checked again afterward. `max_connections` is raised to the selected count;
  the default remains 1,024.
- Ping baseline: 32 and 256 peers continuously send a request and wait for its
  response. This exercises framing, JSON, worker dispatch, and Reactor output
  without a database operation in the measured request.
- Private chat: 16, 64, and 256 peers send bidirectionally in fixed pairs. Each
  producer waits for persistence, receiver delivery, and the receiver's completed
  ACK before sending its next message. The 256-peer case compares four and eight
  TCP workers.
- Group broadcast: 64, 256, and 512 peers form independent 64-member groups.
  One producer per group sends to its 63 other members and waits for all ACKs.
  Therefore a group message and a recipient delivery are different throughput
  units.
- Sustained chat: 64 private-chat peers run for 120 seconds to check completed
  delivery counts and memory behavior over a longer interval.

Busy cases have a three-second warmup and a 20-second measurement window, repeated
three times. The final in-flight operations are drained and included in the
reported elapsed time. Message text contains 128 ASCII bytes; framing and JSON
metadata add more bytes. Group sizes, thread counts, and request limits are
recorded in the result file. The default global unfinished-request limit of
1,024 and per-connection limit of 32 remain enabled.

The load model is **closed loop**. A producer's next operation depends on the
previous one finishing. Percentiles describe the observed workload and do not
estimate latency under an independent, fixed arrival rate or an unbounded queue.

Latency starts before the producer serializes and sends its request:

- Response latency ends when the persistence response arrives.
- Delivery latency ends when a recipient parses the notification.
- Acknowledged latency ends when the producer has its response and all recipient
  ACK responses have arrived. For groups this is the slowest recipient's ACK.

Each notification is checked for sender, recipient, text, group, and message ID.
Final database message and delivery counts must match the completed operations,
including warmup, with zero unacknowledged deliveries. Server warnings and errors
are also checked. Permitted duplicate notifications are counted separately.

CPU time and resident memory are sampled separately for the server, MySQL, and
the load generator. **100% CPU means one logical CPU**, so this eight-CPU machine
can exceed 100% for one process. RSS is process memory and excludes kernel TCP
buffers. MySQL memory and generator memory are not counted as server RSS.
Server file-descriptor and thread counts are captured after connection setup.
The raw JSON contains separate baseline, extended-idle, and lock-profile runs;
supplementary runs are not included in the three-run medians below.

These are same-machine WSL2 measurements. They include contention between MySQL,
the server, and the generator, and do not include a real network's bandwidth,
packet loss, or latency. Connection-holding capacity, ping throughput, and
persisted chat throughput must be quoted separately.

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

## Interpretation and Next Steps

The server held 10,000 authenticated connections with a fixed number of threads
and modest process memory. That connection capacity should not be confused
with the number of clients that can continuously send persisted messages at low
latency. In private chat, moving from 64 to 256 active producers did not raise
four-worker throughput and substantially increased tail latency.

The evidence points to the synchronous database business path as the first
bottleneck in these chat workloads: it is much slower than the database-free
ping baseline, the server CPU remains below one full logical CPU, and increasing
workers improves throughput. Each private message needs a durable message
transaction and a recipient ACK update. Each group message needs its transaction
plus 63 independently persisted ACKs. InnoDB log-buffer waits were zero. Private
chat recorded no row-lock waits; the six multi-group runs recorded 210 row-lock
waits in total, so transaction lock duration also deserves attention. The
measurements do not separately
attribute time to SQL execution, socket round trips, disk synchronization, and
commit scheduling; a claim that any one of those alone is responsible would need
additional profiling.

Two supplementary 20-second group runs measured row-lock wait time explicitly.
The four-group run recorded 20 waits totaling 98 ms; the eight-group run recorded
62 waits totaling 216 ms. These totals are small compared with their measurement
windows. They do not support treating row-lock contention as the main explanation
for the overall throughput limit, although other kinds of database waiting were
not individually timed.

For this host, eight workers improved the 256-peer private-chat median by about
78% and reduced ACK-completion p99. This is an existing configuration choice,
rather than a reason to introduce more concurrency abstractions. It does not
establish an ideal worker count for other machines or for group workloads.

If the project needs more persisted-message throughput, the next small experiment
should batch recipient delivery inserts in one prepared SQL statement. Currently
group persistence performs one insert per recipient. Prepared-statement reuse
could be evaluated after that. Keep measuring completed ACKs, preserve durable
storage settings, and compare the same workloads before accepting an optimization.
These results give no reason to replace the Reactor, add a shared connection
pool, or split the application into more services.

## Describing the Measurements

An accurate connection-capacity statement is:

> Validated 10,000 authenticated TCP connections on an 8-vCPU Linux/WSL2 host
> with four chat workers, 10 total server threads, and approximately 29 MiB of
> server RSS during a 120-second loopback heartbeat test.

For message throughput, keep the worker count and persistence requirements:

> Measured a median 596 fully persisted and recipient-acknowledged private
> messages per second across 256 active peers using eight chat workers and
> durable MySQL writes in loopback benchmarks.

Neither statement establishes a production capacity limit. In particular, the
ping rate should never be presented as persisted chat-message throughput.
