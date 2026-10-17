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
Pool submissions wake workers; completions wake the existing Reactor. These queues
currently have no size limits. The existing frame and connection-output limits
remain in place; queue backpressure is a separate future step.

## Ordering

Only one request per connection is submitted at a time. The front queue entry
remains as an active marker after its payload has been moved into a worker task.
Incoming requests append behind it. A Reactor completion removes that marker and
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
are discarded. A task already submitted to the pool may still finish, but its
completion finds no queue and is ignored. This does not cancel or roll back work
already performed by a handler.

A TCP FIN closes only the peer's sending direction. Clients using piped input still
need their replies after sending FIN. `RequestDispatcher` therefore calls
`defer_close()` while the connection has application work. After the final result,
it calls `resume_close()`, which applies pending sends before allowing EOF to close
the connection. Output that cannot be sent immediately still uses `EPOLLOUT`.

Socket errors such as a reset close the connection immediately. The deferred-close
flag keeps an orderly half-close alive; it does not prevent error cleanup.

## Failures

Malformed envelopes keep the existing protocol error behavior. They are parsed on
a worker and still advance the connection's queue after their error response.

The worker checks the serialized handler response against the 64 KiB frame limit.
An oversized result becomes a correlated `response_too_large` error before it is
posted to the Reactor. This advances the request queue normally and keeps both
the connection and the event loop running. The framing library retains its own
length check for other callers.

An unexpected handler exception is logged and becomes a correlated `internal_error`
response with the message `Request processing failed`. The next request can then
run. The pool also catches exceptions from standalone tasks so one throwing task
does not remove a worker.

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

```bash
./build/tests/chatroom_tests "[thread_pool]"
./build/tests/chatroom_tests "[dispatcher]"
```
