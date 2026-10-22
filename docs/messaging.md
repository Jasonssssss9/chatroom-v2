# Private and Multi-Recipient Messages

`send_message` saves one text message with a delivery for each selected
recipient. The authenticated session supplies the sender. Use
`/send <id[,id...]> <text>` in the console; the [README](../README.md#verify)
includes a two-client walkthrough.

## Send Contract

```json
{"type":"request","request_id":"send-1","action":"send_message","data":{"client_message_id":"alice-1","recipients":["2","3"],"text":"Hello everyone"}}
```

`client_message_id` contains 1–64 bytes, `text` contains 1–4096 UTF-8 bytes,
and `recipients` contains 1–64 positive user ID strings before deduplication.
All recipients must exist; repeated IDs are merged. The sender may be included.
A single ID-only query checks the recipient set.

The message and all deliveries commit in one transaction before success:

```json
{"type":"response","request_id":"send-1","action":"send_message","ok":true,"data":{"message_id":"105"}}
```

Each online recipient receives:

```json
{"type":"notification","action":"message","data":{"message_id":"105","sender_id":"1","client_message_id":"alice-1","text":"Hello everyone"}}
```

A success response confirms persistence, not recipient acceptance. Offline or
expired-session recipients retain pending deliveries. Missing recipients return
`invalid_recipient` without saving a partial send. See
[Protocol](protocol.md#send_message) for envelope and error rules.

## Retries and Duplicate Delivery

Choose a new `client_message_id` for each new outgoing message. To retry an
uncertain result, reuse the same sender, key, text, and deduplicated recipient
set. The database returns the original message ID without creating another
message or delivery. Reusing the key for different content, recipients, a group,
or another message kind returns `message_conflict`.

Private and group messages share one key namespace per sender. The console
generates a fresh key for each command and does not automatically retry sends.
A repeated command therefore creates another message.

A successful retry can emit another online notification. The console suppresses
display for the latest 1024 distinct message IDs and ACKs every copy. Its history
is in memory; restarting the client or evicting an old ID can allow that message
to be displayed again.

## Acknowledgments and Offline Replay

```json
{"type":"request","request_id":"ack-105","action":"ack_message","data":{"message_id":"105"}}
```

Success returns `{"message_id":"105"}` in response data. Repeated ACKs succeed
without changing the first acknowledgment timestamp. Unknown deliveries and
another user's delivery return `invalid_message`. Identity comes from the
session. ACK means client acceptance, not a read receipt or durable local save.

After authentication, the Reactor binds the session and queues the authentication
response before scheduling a replay query on a worker. Pending private messages,
group messages, and file notices share this path.

- Each batch contains at most 16 notifications and 128 KiB of serialized frames,
  including headers. Each payload still has its own 64 KiB limit.
- The server waits for every ID in the current batch to be acknowledged before
  fetching the next batch. Ordinary requests continue while ACKs are pending.
- One missing ACK holds further replay. There is no timed resend; reconnecting
  or authenticating again queries pending deliveries again.
- Every query selects the oldest unacknowledged rows anew. No highest-ID cursor
  can permanently skip a lower ID committed later.

Delivery is at least once across reconnects. Online routing can overlap replay;
disconnects can also lose ACK responses. Clients must ACK duplicate notifications
even when suppressing duplicate display. Socket-send success alone never confirms
a delivery. Keep console stdin open for automatic ACKs.

## Ownership and Failures

A TCP worker validates the request and commits SQL, then returns the response,
serialized notification, and recipient user IDs. The Reactor resolves recipients'
current, unexpired sessions when applying the result. Workers never hold sockets
or online-map references.

A sender disconnect discards its late response while a committed message can
still reach recipients. A recipient changing connections during SQL is resolved
to its current session. Oversized or unserializable results are not routed;
persisted deliveries remain pending.

Replay queries use the same per-connection task sequencing and capacity accounting
as requests. A disconnected connection's late replay result is discarded. Query
or serialization failure logs `message_replay_failed` and closes that connection,
leaving database deliveries available for the next login. Output limits and
server shutdown can likewise discard notifications without deleting pending rows.

See [Threading](threading.md) for ordering and shutdown,
[Groups](groups.md) for group recipient snapshots, and
[Files](files.md#notifications-and-persistence) for file notices.
