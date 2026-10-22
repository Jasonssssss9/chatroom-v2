# Worker Pool and Request Handoff

TCP remains event-driven: one Reactor owns network state, while a fixed pool
runs blocking application work. Four workers are used by default, regardless of
connection count. HTTP has a separate fixed pool.

## Request Flow

1. The Reactor extracts a complete payload and checks admission limits.
2. The dispatcher queues it under its connection ID.
3. If the connection has no active task, its next payload and current session
   snapshot are submitted to the worker pool.
4. A worker parses the request, runs the handler, serializes the result, and
   posts a completion through `TcpServer::post()`.
5. The Reactor applies session changes, queues the response, routes notifications,
   and starts the connection's next task.

| Queue | Ownership and wakeup |
| --- | --- |
| Per-connection payloads | Reactor-only; preserve arrival order |
| Pool tasks | Mutex and condition variable; consumed by fixed workers |
| Reactor completions | Mutex and eventfd; applied on the I/O thread |
| Pending sends | Mutex and eventfd; transferred to connection output buffers |

The pool removes a task under its mutex and releases the lock before executing
it. Completion processing needs no polling thread.

## Request Capacity

Defaults are 32 unfinished requests per connection and 1024 globally.
Configure them through [Configuration](configuration.md). Both must be positive;
the global limit may be smaller than the per-connection limit.

Each admitted request keeps its slot while waiting, submitted, running, or
awaiting a Reactor completion. Only the Reactor changes the counts.
Replay queries also consume one slot.

If a limit is full, the dispatcher logs `request_rejected` and closes the
offending connection with reason `request_limit`. Waiting requests are dropped
and release their slots. Submitted tasks keep their slots until completion,
even after disconnect. Reconnecting therefore cannot create uncounted tasks.

Overload closes the connection without adding another error-response task.
Pending replies are not guaranteed. These limits bound request counts rather
than all memory a handler could allocate. Generic `ThreadPool::submit()` and
`TcpServer::post()`/`send()` have no independent queue limit; another producer
would need its own admission rule.

The network layer checks admission between batches of at most 16 complete
frames, draining full batches without waiting for another ET event.

## Ordering and Ownership

Only one task per connection is submitted at a time. For pipelined requests A
then B, the Reactor applies A's result and queues its response before submitting
B. Authentication therefore binds the session before B receives its snapshot.
Different connections have no global execution or completion order.

A task owns its payload, connection ID, parsed request, and optional session
copy. It holds no socket pointer, connection reference, or reference into the
dispatcher's queues. The handler can run concurrently for different connections;
shared dependencies must support this. Database connections are worker-local.

`RequestResult` contains response JSON and optional authentication,
notification, or ACK effects. Workers serialize notifications and return
recipient user IDs. The Reactor checks expiry again before binding identities
and resolves recipients' current sessions when routing. Expired or offline
recipients remain pending in MySQL.

Completions look up their connection ID again. Only Reactor callbacks modify
connection, request, and session maps. See [Networking](networking.md) and
[Database](database.md).

## Disconnects and EOF

Disconnect removes the session and waiting payloads. Submitted work can still
commit. Its completion releases its slot and discards the closed connection's
response and authentication effect; a committed notification can still reach
other users. Disconnect does not cancel or roll back work already executing.

An orderly FIN retains the connection while complete requests run, using
`defer_close()` and `resume_close()`. Final output flushes before close.
Resets close immediately. A trailing incomplete frame is logged and excluded
from dispatch without losing replies to preceding complete frames.

## Errors

All responses pass through the same serialization and 64 KiB check, including
`ProtocolError` responses and exception fallbacks. An oversized response becomes
a correlated `response_too_large` error. Oversized notifications are not routed.
Failed serialization and unexpected handler exceptions become
`internal_error` with `Request processing failed`.

Error responses advance the queue normally, allowing the next request to run.
The pool also catches standalone task exceptions so a throwing task does not
remove a worker. Effects from failed or oversized results are discarded; already
committed database records remain intact.

Reactor callbacks must be short and non-blocking. An unexpected exception from
`post()` callbacks leaves `run()` after network cleanup.

## Replay Tasks

The first query runs after the Reactor binds the authenticated session. Workers
return at most 16 serialized notifications totaling 128 KiB with frame headers.
The dispatcher validates these limits before posting the result.

A query is the connection's sole active task. After it finishes, normal requests
resume while the Reactor retains only the current batch's IDs. Successful ACKs
remove IDs; acknowledging the last schedules the next query. Duplicate or
unrelated ACKs do not change the window. Waiting for ACKs consumes no worker.

Late results for a disconnected connection are discarded. Online sends can
overlap replay, so clients deduplicate display and ACK every copy. Query or
serialization failures close only that connection and retain pending records.
See [Messaging](messaging.md#acknowledgments-and-offline-replay).

## Shutdown

Dependencies must outlive their tasks and callbacks. Construct the server before
the dispatcher; stop and join a separately running Reactor before destroying the
dispatcher.

The executable handles SIGINT and SIGTERM through signalfd:

1. Stop the Reactor, close connections, and drop waiting requests and queued output.
2. Stop and join HTTP while its dependencies still exist.
3. Destroy the dispatcher, drain already submitted TCP tasks, and join its pool.
4. Destroy services and the server, then log `server_stopped`.

The pool drains accepted tasks, not payloads still waiting in per-connection
queues. Workers must eventually return for joining to finish. Shutdown does not
guarantee pending response delivery; persisted unacknowledged messages replay
after authentication.
