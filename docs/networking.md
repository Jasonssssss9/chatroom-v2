# Networking

## Components

| Component | Responsibility |
| --- | --- |
| `UniqueFd` | Owns a file descriptor and closes it on destruction; moving transfers ownership |
| `Socket` | Creates IPv4 listeners, accepts connections, and wraps Linux receive/send calls |
| `Epoll` | Owns the epoll instance and registers, updates, removes, and waits for events |
| `FrameDecoder` | Extracts length-prefixed payloads while retaining incomplete input |
| `RequestHandler` | Validates and dispatches application JSON requests separately from socket I/O |
| `RequestDispatcher` | Submits one request per connection to workers and applies completions on the Reactor |
| `TcpConnection` | Owns one client's socket, input decoder, and buffered output |
| `TcpServer` | Runs the Reactor, manages connections, and invokes application callbacks |
| `TcpClient` | Owns one blocking IPv4 connection and sends or receives complete frames |
| `AuthClient` | Performs HTTP registration and login on the input thread |
| `ConsoleClient` | Handles commands, reply display, ACKs, heartbeats, and thread shutdown |

The implementation uses Linux socket APIs directly. Server sockets are non-blocking
and close on exec. ET handlers accept, read, or write until `EAGAIN`; `EINTR` retries
the interrupted operation. Writes use `MSG_NOSIGNAL` so a closed peer cannot terminate
the process with `SIGPIPE`. The executable uses the [JSON protocol](protocol.md)
on top of this byte-oriented network layer; network tests may use simple echo
callbacks independently of the application handler.

Startup and epoll failures use `std::system_error`. A connection's read, write,
framing, or message-handler failure is reported as a JSON log on standard error
and closes that connection. Connection events include the connection ID and peer
address so request errors can be traced back to a client. Disconnect handlers
should not throw. See [Configuration and Logging](configuration.md).

## Accept Errors and Connection Capacity

The server keeps at most `server.max_connections` active connections, defaulting
to 1024. A newly accepted socket above that limit is closed immediately and logged
as `connection_rejected`. Existing clients keep their connections. Rejected sockets
are never registered with epoll or passed to application handlers.

Choose this limit below the process's available file-descriptor budget. The listener,
epoll instance, eventfd, standard streams, and future database or file operations
also need descriptors. The setting does not change the operating system's FD limit.

`EINTR`, aborted connections, and pending network errors from `accept4()` retry the
accept loop. `EAGAIN` ends the current batch. If `accept4()` reports `EMFILE`,
`ENFILE`, `ENOBUFS`, or `ENOMEM`, the Reactor records `accept_paused`, removes the
listener from epoll, and schedules another attempt after 200 ms. Other errors still
leave `run()` as exceptions.

The Reactor uses the nearest idle-scan or accept-retry deadline for `epoll_wait()`.
It continues
to process established connections, completions, and `stop()` notifications; it
does not sleep or start a timer thread. The deadline is checked after every wait,
including waits that return other events. At the deadline it restores the listener
and explicitly calls `accept4()` again, so recovery does not depend on a new ET
notification. Another resource failure schedules another delayed attempt.

During a resource pause, new clients may wait in the kernel backlog even if their
TCP connect succeeds. They can exchange application data after accepting recovers.
The connection limit and resource retry handle connection admission. Application
request admission is handled by the [dispatcher limits](threading.md#request-capacity).

## Framing and Limits

A frame has a four-byte unsigned length in network byte order followed by exactly
that many payload bytes. Payloads are limited to 64 KiB. The framing library handles
bytes, including empty payloads and embedded zero bytes; the application validates
JSON separately.

The decoder accepts fragmented headers, fragmented payloads, and multiple frames
in one receive. `TcpConnection::receive()` returns at most 16 frames at a time.
After a full batch, the Reactor calls it again immediately, processing frames
already in the decoder before reading more bytes. It continues until `EAGAIN` or
EOF, allowing request limits to be checked between batches. If a message callback
closes the connection, the Reactor discards the rest of that batch and stops reading
it. A direct caller of `receive()` must also drain full batches before waiting for
another readiness event.

Consuming a frame never discards trailing input. Each connection
limits its unsent output buffer to 1 MiB; exceeding that limit closes the connection.
`EPOLLOUT` is enabled only while output remains buffered.

## Thread Ownership

Configure the message and disconnect handlers before calling `run()`. They execute
on the Reactor thread and should return promptly. The executable wires them to
`RequestDispatcher`, which runs parsing, application work, and serialization on a
fixed worker pool. See [Worker Pool and Request Handoff](threading.md).

Only the Reactor thread accesses connections and their buffers. `send(id, payload)`
can be called from another thread: it copies and frames the payload, puts it in a
mutex-protected queue, and wakes the loop through `eventfd`. Connection IDs increase
for each accepted connection and are stored in epoll events. A delayed result for
a closed connection is discarded even if its socket number has been reused.

`send()` queues output; it does not confirm delivery to the peer. Senders that need
an application-level ordering rule must submit their results in that order. The
dispatcher guarantees this for requests on one connection.

`post(callback)` queues a short callback on the same eventfd-backed wakeup path.
Callbacks execute on the Reactor and must not perform blocking work. Captured
objects must outlive the event loop. Application completions use this API to look
up the connection ID again and advance the corresponding request queue.

`close_connection(id, reason)` is Reactor-only. It removes the connection and
invokes the disconnect handler immediately. The dispatcher uses it for overload
cleanup; worker threads must not call it.

## Connection and Server Shutdown

When the peer shuts down its write side, complete final frames are processed.
The dispatcher calls `defer_close()` while application requests remain, so EOF
does not drop their asynchronous replies. Its final completion calls `resume_close()`
to flush queued output and allow the connection to close. These two methods are
Reactor-only. Socket errors still close the connection immediately.

At EOF, `TcpConnection` checks whether the decoder still holds an incomplete
header or payload. It marks `truncated_frame()` without throwing away complete
frames from the same batch. The Reactor logs one `connection_error` warning with
code `truncated_frame`, connection ID, and peer address. Only complete frames reach
the application. Their pending replies follow the same deferred-close path; after
they are flushed, the connection closes with reason `truncated_frame` instead of
`peer_closed`. EOF between frames remains an ordinary half-close without a warning.
The incomplete trailing bytes do not receive a JSON error response.

Call `stop()` from a normal thread to wake the loop and stop it. Shutdown closes
active connections and does not drain pending output. Run each server instance
once. Pending callbacks are discarded, and later `send()` and `post()` calls are
ignored. Join the loop and worker threads before destroying the server.

The executable blocks SIGINT and SIGTERM before creating workers, which inherit
the signal mask. A non-blocking, close-on-exec `signalfd` turns these signals into
an epoll event passed to `run()`. The Reactor logs `shutdown_requested` and calls
`stop()` on its own thread. No asynchronous signal handler calls application code.
The mask stays blocked through teardown, so another signal does not interrupt
cleanup. After the Reactor closes connections, the executable stops and joins
the HTTP service, then destroys the dispatcher and waits for submitted TCP tasks.
Requests still waiting in per-connection queues are discarded. `server_stopped`
is logged after all workers have joined, and the process exits successfully.
Queued responses and notifications are not drained during shutdown; persisted
deliveries remain available for later acknowledgment or replay.

## Client Networking

`TcpClient` connects in its constructor. One thread may call `send()` while another
calls `receive()`. Multiple concurrent senders or receivers are not supported.
The library handles framing without interpreting JSON. It retries interrupted I/O,
completes partial writes, and retains incomplete or consecutive replies. A clean
EOF returns no message; EOF in the middle of a frame reports an error.

`finish_sending()` sends FIN while keeping the receive direction open. `disconnect()`
uses `shutdown(SHUT_RDWR)` to cancel blocked I/O without closing a descriptor that
another thread is using. Join I/O threads before destroying the client. Create a
new instance to reconnect. Connection establishment is blocking and uses the
operating system's timeout; hostnames and automatic reconnect are not implemented.

TCP sends have a five-second socket timeout. The console's timer sends automatic
pings independently of input and HTTP operations and cancels TCP on a missing
matching pong. A periodic Reactor scan closes idle connections without an extra
server thread. See [Connection Keepalive](keepalive.md) for both timeouts.

The console limits each input line to 64 KiB before JSON validation and sending.
LF and an optional preceding CR do not count toward that limit. On overflow it
reports one error immediately and discards the rest of that line, retaining only
a bounded input buffer. The next line is processed normally; an oversized final
line is also discarded at EOF. A receive thread
prints replies and signals an eventfd when it finishes. The input thread polls
standard input and that eventfd, so remote disconnects also wake terminal input.
Output is serialized with a mutex. `/quit` cancels I/O immediately; standard-input
EOF finishes sending and waits for the peer to close after its final replies.
Ctrl+C uses normal process termination.

Commands use an `AuthClient` for HTTP registration and login. The input thread
performs these blocking calls while the receiver continues processing messages.
Login returns a JWT directly to a generated TCP `authenticate` request. The
client does not display or persist the token. `/send` and `/send_group` generate
fresh client message IDs. `/create_group` and `/members` manage group creation
and member queries; `/ping`, `/help`, and `/quit` handle connection checks and
local control. Normal
output formats messages and errors; `--json` retains raw payload display.
See [Command-Line Client](client.md) for syntax and examples.

The receiver recognizes `message` and `file_available` notifications, suppresses recent duplicate IDs,
and sends `ack_message` after display. It acknowledges duplicate copies too. A
send mutex serializes automatic ACKs with terminal requests and the EOF half-close;
the underlying `TcpClient` still requires one sender at a time. The receive thread
alone owns the bounded history of 1,024 IDs. Server JSON is parsed with the shared
nesting limit; unexpected payloads are displayed without automatic acknowledgment.
After stdin EOF, notifications can still be displayed but cannot be acknowledged
on the half-closed socket. Keep stdin open to receive and acknowledge replay batches.

## Tests

Catch2 tests use local TCP connections and socket pairs. They cover partial frames,
consecutive messages, UTF-8 byte lengths, embedded zero bytes, frame limits, partial
writes, output limits, disconnected peers, half-close handling, stale replies,
cross-thread sends, and loop shutdown. Integration tests bind to port `0` and use
socket timeouts and futures rather than timing sleeps.

EOF tests cover partial headers, partial payloads, clean half-closes, and complete
requests followed by a truncated frame at or beyond a receive-batch boundary.
They verify ordered worker replies, continued service for other clients, and
exactly one warning with the correct close reason.

Connection-capacity tests verify that excess clients are closed and new clients
can connect after an active one leaves. FD-exhaustion tests lower `RLIMIT_NOFILE`
only in an isolated child process. They fill its descriptor table, wait for actual
pause logs, exercise established I/O and repeated retries, then verify backlog
recovery and stopping while acceptance remains paused.

Client tests use a raw TCP peer and can run with `BUILD_SERVER=OFF`. They cover
framed sends, fragmented and consecutive replies, truncated or oversized frames,
send limits, final replies after FIN, cancellation, and connection failures.
