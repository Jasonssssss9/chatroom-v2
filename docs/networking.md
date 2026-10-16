# Networking

## Components

| Component | Responsibility |
| --- | --- |
| `UniqueFd` | Owns a file descriptor and closes it on destruction; moving transfers ownership |
| `Socket` | Creates IPv4 listeners, accepts connections, and wraps Linux receive/send calls |
| `Epoll` | Owns the epoll instance and registers, updates, removes, and waits for events |
| `FrameDecoder` | Extracts length-prefixed payloads while retaining incomplete input |
| `TcpConnection` | Owns one client's socket, input decoder, and buffered output |
| `TcpServer` | Runs the Reactor, manages connections, and invokes application callbacks |

The implementation uses Linux socket APIs directly. Server sockets are non-blocking
and close on exec. ET handlers accept, read, or write until `EAGAIN`; `EINTR` retries
the interrupted operation. Writes use `MSG_NOSIGNAL` so a closed peer cannot terminate
the process with `SIGPIPE`.

Startup and epoll failures use `std::system_error`. A connection's read, write,
framing, or message-handler failure is reported to standard error and closes that
connection. Disconnect handlers should not throw.

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
on the Reactor thread and should return promptly. Future business handlers will
submit database work to a separate thread pool.

Only the Reactor thread accesses connections and their buffers. `send(id, payload)`
can be called from another thread: it copies and frames the payload, puts it in a
mutex-protected queue, and wakes the loop through `eventfd`. Connection IDs increase
for each accepted connection and are stored in epoll events. A delayed result for
a closed connection is discarded even if its socket number has been reused.

`send()` queues output; it does not confirm delivery to the peer. Senders that need
an application-level ordering rule must submit their results in that order.

## Connection and Server Shutdown

When the peer shuts down its write side, complete final frames are processed and
already queued replies are flushed before the server closes the socket. An EOF
does not keep the connection alive for future asynchronous replies. Clients should
keep both directions open while awaiting asynchronous results.

Call `stop()` from a normal thread to wake the loop and stop it. Shutdown closes
active connections and does not drain pending output. Run each server instance
once. Stop producers and join the loop thread before destroying the server.
`stop()` is not a signal-handler API; the echo executable currently uses normal
process termination for Ctrl+C.

## Tests

Catch2 tests use local TCP connections and socket pairs. They cover partial frames,
consecutive messages, UTF-8 byte lengths, embedded zero bytes, frame limits, partial
writes, output limits, disconnected peers, half-close handling, stale replies,
cross-thread sends, and loop shutdown. Integration tests bind to port `0` and use
socket timeouts and futures rather than timing sleeps.
