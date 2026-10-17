# Worker Pool and Request Handoff

The server uses one Reactor thread and a fixed worker pool. Socket I/O remains
event-driven. JSON parsing, application handlers, and response serialization run
as worker tasks. The executable starts four workers by default; JSON configuration
or `--workers` can change the fixed pool size. See [Configuration and Logging](configuration.md).

## Responsibilities

| Component | Responsibility |
| --- | --- |
| `TcpServer` | Socket I/O, connection ownership, and callbacks posted to the Reactor |
| `ThreadPool` | A FIFO task queue, fixed workers, and shutdown that joins them |
| `RequestDispatcher` | Per-connection sequencing and handing results back to the Reactor |
| `RequestHandler` | Operations on validated requests, currently `ping` |

The pool knows nothing about connections, JSON, or chat operations. Its workers
wait on a condition variable when the task queue is empty. They release the queue
mutex before executing a task. Different connections can use different workers.
The number of threads does not grow with the number of clients.

## Request Flow

```text
Reactor: receive bytes and extract complete frames
    -> append the payload to that connection's request queue
    -> submit its first request to the worker pool

Worker: parse the request envelope
    -> execute the handler
    -> serialize the response
    -> post a completion through TcpServer::post()

Reactor: wake through eventfd and apply the completion
    -> queue the response for socket output
    -> submit the next request for that connection
```

There are four ordinary queues with different jobs:

| Queue | Access | Purpose |
| --- | --- | --- |
| Per-connection requests | Reactor only | Keep requests from one client in order |
| Pool tasks | Mutex and condition variable | Assign runnable work to fixed workers |
| Reactor completions | Mutex and eventfd | Return results to the owning I/O thread |
| Pending sends | Mutex and eventfd | Transfer serialized output to connection buffers |

The application request queues and completion queue do not need a polling thread.
Pool submissions wake workers; completions wake the existing Reactor. Admission
limits in the dispatcher bound this request pipeline, including pool tasks and
completions awaiting the Reactor. The existing frame and connection-output limits
remain in place.

## Request Capacity

The defaults are 32 unfinished requests per connection and 1024 across the server.
Configure them with `server.max_requests_per_connection` and
`server.max_pending_requests`, or the corresponding command-line options. Both
limits must be positive; the global limit may be smaller than the per-connection
limit. See [Configuration and Logging](configuration.md).

An admitted request keeps one global slot while it is waiting in a connection's
queue, submitted to the pool, running, or awaiting a Reactor completion. Only the
Reactor changes this count. Completion releases the slot even if the connection
has already disappeared.

If either limit is full, the dispatcher logs `request_rejected` and closes the
offending connection with reason `request_limit`. Its waiting requests are dropped
and their slots are released immediately. Already submitted tasks retain their
slots until their completions arrive. Repeatedly disconnecting and reconnecting
therefore cannot fill the pool with uncounted tasks.

This is an overload-close policy. Pending replies on that connection are not
guaranteed, and no extra error-response task is queued during overload. The JSON
envelope and error-code set are unchanged by admission limits.

The dispatcher owns the application's pool and is its only producer, so the pool
does not need a second capacity policy. The generic `ThreadPool::submit()` and
`TcpServer::post()`/`send()` APIs have no independent queue limits; other future
producers must apply their own admission rules. These limits bound request counts,
not total process memory or the size of arbitrary objects created by handlers.

The network layer returns at most 16 complete frames per receive batch, then lets
the dispatcher check admission. Full batches are followed by another receive call
until `EAGAIN` or EOF. This bounds temporary frame collection without leaving data
waiting for a new ET notification.

## Ordering

Only one request per connection is submitted at a time. Each connection stores a
queue of waiting payloads and an `active` flag for its submitted task. Incoming
requests append to the queue. After successful submission, the payload is removed
from that queue and belongs to the task. A Reactor completion clears the flag and
then starts the next request.

For example, if one connection sends A followed by B:

1. A starts; B waits in the application queue.
2. A finishes on a worker and posts its result.
3. The Reactor applies A's result and queues its response.
4. Only then is B submitted.

This orders both execution and responses on one connection. It also leaves a clear
place to apply future authentication state before starting the next request.
Requests from different connections have no global execution or completion order.

## Data Ownership

A worker task owns its payload string and carries the connection ID by value.
It parses that payload into its own `Request`. It never holds a `TcpConnection&`,
socket pointer, or reference to the dispatcher's per-connection queue.

The handler receives the connection ID and a const reference to that task's local
request. It may be invoked concurrently for different connections, so shared
service dependencies must support concurrent calls. The current `RequestHandler`
is stateless. Future database handlers will acquire their own database connection
for each operation rather than sharing a raw connection between workers.

Only Reactor callbacks modify the dispatcher's request map and network state.
Completions look up the ID again. They do not reuse a connection reference captured
when the request arrived.

## Disconnects and EOF

A disconnect callback erases that connection's application queue. Waiting requests
are discarded and release their global slots. A task already submitted to the pool
may still finish. Its completion releases its slot, finds no connection queue, and
discards the response. This does not cancel or roll back work already performed by
a handler.

A TCP FIN closes only the peer's sending direction. Clients using piped input still
need their replies after sending FIN. `RequestDispatcher` therefore calls
`defer_close()` while the connection has application work. After the final result,
it calls `resume_close()`, which applies pending sends before allowing EOF to close
the connection. Output that cannot be sent immediately still uses `EPOLLOUT`.

Socket errors such as a reset close the connection immediately. The deferred-close
flag keeps an orderly half-close alive; it does not prevent error cleanup.

An EOF with an incomplete trailing frame is logged as `truncated_frame`. The
connection records this state while returning all complete frames already read.
Those requests enter the dispatcher normally, including when the last complete
frame ends exactly on a receive-batch boundary. The existing deferred-close path
lets their workers finish and flush replies before closing with the truncation
reason. The partial frame is never submitted as a task or parsed as JSON.

## Failures

Malformed envelopes keep the existing protocol error behavior. They are parsed on
a worker and still advance the connection's queue after their error response.

All worker responses use the same serialization and 64 KiB size check, including
responses built from `ProtocolError` and the `internal_error` fallback. An oversized
result becomes a correlated `response_too_large` error before it is posted to the
Reactor. The limit counts serialized bytes, including escaped error messages;
exactly 65,536 bytes is allowed. This advances the request queue normally and keeps
both the connection and the event loop running. The framing library retains its
own length check for other callers.

An unexpected handler exception or response serialization failure is logged and
becomes a correlated `internal_error` response with the message
`Request processing failed`. This also covers invalid UTF-8 in a `ProtocolError`
message. The next request can then run. The pool also catches exceptions from
standalone tasks so one throwing task does not remove a worker.

`post()` callbacks must remain short and non-blocking. Unexpected exceptions from
these callbacks leave `run()` after connection and pending-queue cleanup. Blocking
service work belongs in the worker handler, not a completion callback.

## Lifetime and Shutdown

Construct the server before the dispatcher, install its message and disconnect
callbacks, then call `run()`. When using a separate Reactor thread, stop and join
that thread before destroying the dispatcher.

Shutdown proceeds as follows:

1. `stop()` wakes the Reactor and stops network processing.
2. Reactor cleanup closes connections and invokes their disconnect callbacks.
3. Queued sends and completions are discarded; later `send()` and `post()` calls
   are ignored.
4. Destroying the dispatcher shuts down its pool, finishes already submitted
   tasks, and joins workers while the handler and server still exist.
5. The server and service dependencies can then be destroyed.

The pool drains its own accepted tasks. Requests still waiting in per-connection
queues are not pool tasks and are discarded during disconnect cleanup. Shutdown
does not promise delivery of pending responses. A handler that blocks indefinitely
also prevents its worker from joining; handlers must eventually return.

## Verification

The tests use promises and shared futures rather than timing sleeps. A blocked
handler proves that another connection can still receive a response, while a
second request on the blocked connection cannot start early. Other tests cover
ordered replies after FIN, resets during active work, handler exceptions, shutdown
with an active task, fixed worker count, and draining the pool queue.
Capacity tests also cover overload isolation, dropping waiting requests, retaining
submitted tasks after disconnect, reusing released slots, and errors with a budget
of one request.

```bash
./build/tests/chatroom_tests "[thread_pool]"
./build/tests/chatroom_tests "[dispatcher]"
```
