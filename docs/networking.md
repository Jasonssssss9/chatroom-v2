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
| `ConsoleClient` | Coordinates terminal input, reply display, and receiver thread shutdown |

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

## Framing and Limits

A frame has a four-byte unsigned length in network byte order followed by exactly
that many payload bytes. Payloads are limited to 64 KiB. The framing library handles
bytes, including empty payloads and embedded zero bytes; the application validates
JSON separately.

The decoder accepts fragmented headers, fragmented payloads, and multiple frames
in one receive. Consuming a frame never discards trailing input. Each connection
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

## Connection and Server Shutdown

When the peer shuts down its write side, complete final frames are processed.
The dispatcher calls `defer_close()` while application requests remain, so EOF
does not drop their asynchronous replies. Its final completion calls `resume_close()`
to flush queued output and allow the connection to close. These two methods are
Reactor-only. Socket errors still close the connection immediately.

Call `stop()` from a normal thread to wake the loop and stop it. Shutdown closes
active connections and does not drain pending output. Run each server instance
once. Pending callbacks are discarded, and later `send()` and `post()` calls are
ignored. Join the loop and worker threads before destroying the server.
`stop()` is not a signal-handler API; the server executable currently uses normal
process termination for Ctrl+C.

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

The console validates each input line as JSON, then sends it. A receive thread
prints replies and signals an eventfd when it finishes. The input thread polls
standard input and that eventfd, so remote disconnects also wake terminal input.
Output is serialized with a mutex. `/quit` cancels I/O immediately; standard-input
EOF finishes sending and waits for the peer to close after its final replies.
Ctrl+C uses normal process termination.

## Tests

Catch2 tests use local TCP connections and socket pairs. They cover partial frames,
consecutive messages, UTF-8 byte lengths, embedded zero bytes, frame limits, partial
writes, output limits, disconnected peers, half-close handling, stale replies,
cross-thread sends, and loop shutdown. Integration tests bind to port `0` and use
socket timeouts and futures rather than timing sleeps.

Client tests use a raw TCP peer and can run with `BUILD_SERVER=OFF`. They cover
framed sends, fragmented and consecutive replies, truncated or oversized frames,
send limits, final replies after FIN, cancellation, and connection failures.
