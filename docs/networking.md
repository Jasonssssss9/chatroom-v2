# Networking

The network layer uses Linux socket APIs directly. The server runs a single
edge-triggered epoll Reactor; application work runs on workers.

## Components

| Component | Responsibility |
| --- | --- |
| `UniqueFd` | Move-only descriptor ownership and automatic close |
| `Socket` | IPv4 listeners, accepted sockets, and send/receive calls |
| `Epoll` | Event registration, updates, removal, and waits |
| `FrameDecoder` | Length-prefixed payloads and incomplete input |
| `TcpConnection` | One socket, input decoder, and buffered output |
| `TcpServer` | Reactor, connections, application callbacks, and cross-thread wakeups |
| `TcpClient` | One blocking connection with framed send/receive |

Server sockets are non-blocking and close-on-exec. Accept, read, and write
handlers drain until `EAGAIN`; `EINTR` retries the operation. Writes use
`MSG_NOSIGNAL` to avoid process termination when a peer closes.

Startup and epoll failures throw `std::system_error`. Connection-level I/O,
framing, or message-callback failures are logged and close the affected
connection. Disconnect callbacks must not throw.

## Accept Errors and Connection Capacity

The default active-connection limit is 1024. An accepted socket above the limit
is closed and logged as `connection_rejected`, without reaching epoll or
application handlers. Existing connections continue normally.

This limit does not raise the process's file-descriptor limit. Leave capacity for
listeners, epoll, eventfd, database connections, and open files.

The accept loop retries interrupted, aborted, and pending network errors.
`EAGAIN` finishes the batch. Resource errors `EMFILE`, `ENFILE`, `ENOBUFS`,
and `ENOMEM` log `accept_paused`, remove the listener from epoll, and schedule
a retry after 200 ms. Other errors leave `run()` as exceptions.

The retry shares the epoll timeout with idle scans, so existing I/O, completions,
and stop requests continue. At the deadline, the Reactor restores the listener
and explicitly accepts again; recovery does not depend on another ET event.
A repeated resource failure schedules another retry. New clients can wait in
the kernel backlog during the pause even if their TCP connect succeeds.

Application request limits are separate; see
[Request Capacity](threading.md#request-capacity).

## Framing and Buffered I/O

A frame contains a four-byte unsigned big-endian length and that many bytes,
with a maximum payload of 65536 bytes. The framing layer accepts empty and
binary payloads; the application separately requires UTF-8 JSON.

The decoder handles fragmented headers, fragmented bodies, and multiple frames
per read without dropping trailing input. `TcpConnection::receive()` returns at
most 16 complete frames per batch. After a full batch, the Reactor immediately
calls again until `EAGAIN` or EOF, checking request admission between batches.
Direct users of `receive()` must follow the same draining rule. If a callback
closes the connection, the remaining batch is discarded.

Each connection permits at most 1 MiB of unsent output. Exceeding it closes that
connection. `EPOLLOUT` is enabled only while output is pending. Complete
incoming frames refresh the idle deadline; see [Keepalive](keepalive.md).

## Reactor Ownership

Install handlers before `run()`. Message and disconnect handlers execute on
the Reactor thread and must return promptly. The application's message callback
hands payloads to `RequestDispatcher` for worker-side processing.

| API | Thread contract |
| --- | --- |
| `send(id, payload)` | Thread-safe; frames and queues output, then wakes eventfd |
| `post(callback)` | Thread-safe; queues a short Reactor callback through eventfd |
| `stop()` | Thread-safe; wakes and stops the loop |
| `close_connection(id, reason)` | Reactor-only; closes and invokes disconnect immediately |
| `defer_close(id)`, `resume_close(id)` | Reactor-only; retain a half-closed connection for final replies |

Only the Reactor accesses connections and their buffers. Monotonically increasing
connection IDs are stored in epoll events, so a late result cannot reach a new
connection that reused an old socket number. Sends for closed IDs are discarded.

`send()` confirms queueing, not delivery. Producers must supply any required
ordering; the dispatcher does so per connection. `post()` callbacks must not
block, and captured dependencies must outlive the loop. Generic cross-thread
queues have no independent capacity limit; application admission rules bound
the request pipeline.

## EOF and Shutdown

An orderly FIN closes only the peer's sending direction. Complete final frames
still enter dispatch. While work remains, `defer_close()` retains the connection;
the final completion calls `resume_close()` to flush queued replies before
closing. Socket errors, including resets, close immediately.

EOF with an incomplete trailing header or payload logs one `truncated_frame`
warning. Complete preceding frames still receive ordered replies, including at
receive-batch boundaries. The partial frame is never parsed and receives no
JSON error. The final close reason is `truncated_frame`; EOF between complete
frames closes normally as `peer_closed`.

Run a server instance once. `stop()` closes active connections and discards
pending output and callbacks; later `send()` and `post()` calls are ignored.
Join the event loop and all producers before destroying the server.

The executable blocks SIGINT and SIGTERM before creating workers. A
non-blocking `signalfd` passes those signals to epoll, where they log
`shutdown_requested` and call `stop()`. No asynchronous signal handler runs
application code. After the Reactor returns, HTTP stops and joins, then the
dispatcher drains submitted TCP tasks and joins workers. Waiting per-connection
requests are discarded. `server_stopped` follows worker shutdown.

Shutdown does not promise to flush replies or notifications. Persisted,
unacknowledged deliveries remain available after reconnecting.

## Client Networking

`TcpClient` connects in its constructor using numeric IPv4. Establishment is
blocking and uses the operating system's timeout. Create a new instance to
reconnect.

One thread may send while another receives; multiple concurrent senders or
receivers require external serialization. The library completes partial I/O,
retries interruptions, and retains incomplete or consecutive frames. Clean EOF
returns no message; EOF inside a frame reports an error. Writes have a
five-second socket timeout.

`finish_sending()` uses `SHUT_WR` while retaining receive access.
`disconnect()` uses `SHUT_RDWR` to cancel blocked I/O without closing a
descriptor another thread is using. Join I/O threads before destruction.

The console serializes sends and polls stdin alongside a receiver-completion
eventfd. It joins the receiver and timer threads before returning. See
[Client](client.md) for commands, input limits, cancellation, and ACK behavior.
