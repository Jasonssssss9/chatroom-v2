# Private and Multi-Recipient Messages

The server supports persisted text messages between authenticated TCP clients.
Private messages and multi-recipient sends use the same `send_message` action.
The command-line client provides chat commands, readable messages, and automatic
acknowledgments. Use `--json` when you need raw TCP payloads.

## Try Two Clients

Start the server with MySQL and JWT environment variables configured as described
in [Authentication](authentication.md). Its default HTTP and TCP ports are 8080
and 8081.

In Alice's terminal, start the client:

```bash
./build/client/chatroom_client 127.0.0.1 8081
```

Register and log in as Alice:

```text
/register alice example-password
/login alice example-password
```

The client prints Alice's user ID after registration and confirms TCP
authentication after login. It handles HTTP and the JWT automatically.

Start a second client in Bob's terminal:

```bash
./build/client/chatroom_client 127.0.0.1 8081
```

```text
/register bob example-password
/login bob example-password
```

Use the actual registration IDs below. If Alice's ID is 1 and Bob's is 2, Alice
can send:

```text
/send 2 Hello Bob
```

Alice sees the persisted message ID:

```text
Sent message 105.
```

Bob receives the message and automatically acknowledges it:

```text
[message 105] user 1: Hello Bob
```

Bob can reply:

```text
/send 1 Hi Alice
```

Use `/quit` to exit and `/login` after reconnecting. If a token expires, use
`/login` again for the same account. For a different HTTP port, supply it as the
third positional argument. See [Command-Line Client](client.md) for quoting and
display modes, and [JSON Protocol](protocol.md) for wire-format examples.

## Multiple Recipients and Retries

Separate recipient IDs with commas:

```text
/send 2,3 Hello everyone
```

Both users must exist. They share one stored message and message ID, with a
delivery row for each recipient. Repeated IDs are merged. You may include your
own ID. Online recipients get notifications; offline deliveries stay in MySQL.
See [JSON Protocol](protocol.md#send_message) for field limits and error codes.

The `/send` command generates a fresh client message ID automatically. In raw
JSON clients, choose a new `client_message_id` for each new outgoing message. For an uncertain
send result, retry with the same client ID, text, and recipient set. The response
uses the existing message ID; it does not create another message or delivery.
Notifications can repeat. The console client displays a message once while its
ID remains in a bounded history of the most recent 1,024 distinct message IDs,
and automatically acknowledges every copy. This history is in memory only;
restarting the client or evicting an old ID can allow it to be displayed again.
Keep stdin open for automatic ACKs. EOF half-closes the sending side, displays
final notifications without ACKs, and lets the server close the connection.

## Acknowledgments and Offline Replay

The server keeps a delivery pending until its recipient sends `ack_message`:

```json
{"type":"request","request_id":"ack-105","action":"ack_message","data":{"message_id":"105"}}
```

Success echoes `message_id`. Repeating an ACK succeeds; another user's delivery
or an unknown ID receives `invalid_message`. The recipient comes from the session.
ACK confirms client acceptance rather than a read receipt or durable local storage.

To try offline delivery, disconnect Bob, send him a message as Alice, then reconnect
and authenticate Bob. The authentication response is followed by the pending
`message` notifications. The console displays them and sends ACKs automatically.
After those ACKs succeed, another reconnect does not replay the same deliveries.
Raw JSON clients, such as custom test peers, must send their own ACKs.

Replay uses batches of at most 16 notifications and 128 KiB of serialized frames.
The server waits for the current batch's IDs to be acknowledged before querying
the next batch. Count and byte limits are independent, and JSON escaping counts
toward the byte limit. Each notification still has its own 64 KiB payload limit.
One missing ACK holds up further replay; reconnecting or authenticating again
requeries unacknowledged deliveries. There is no timed resend loop.

This provides at-least-once delivery across reconnects. An online message can also
appear in a replay query, and a disconnect can lose an ACK response. Clients
must acknowledge duplicate notifications as well as suppressing duplicate display.
The server selects unacknowledged rows on every query instead of keeping a
highest-ID cursor, so a lower ID committed later is not permanently skipped.

## Persistence and Thread Ownership

1. A TCP worker reads the sender from its session snapshot and validates the data.
2. It checks recipients and commits the message and deliveries in MySQL.
3. It returns the response and one serialized notification with recipient user IDs.
4. The Reactor resolves each recipient's current, unexpired session and queues output.

Workers do not access sockets or online maps. A disconnected sender's late
response is discarded, while its committed message can still notify recipients.
Changing the recipient's connection during database work routes the notification
to the new connection. A failed or oversized result is not routed.

The Reactor binds a successful authentication before submitting the first replay
query, so concurrent sends can already resolve the recipient's online session.
Replay queries and ACK writes use the same per-connection worker sequencing as
ordinary requests. Each replay query consumes one request-capacity slot. The
Reactor keeps only the current replay batch's IDs and clears them on disconnect;
late replay results cannot access or deliver to a replacement connection.

A replay query or serialization failure logs `message_replay_failed` and closes
that connection. Already persisted deliveries remain pending for reconnect.
Socket output limits and shutdown can also discard notifications without losing
their database records. Socket send success alone never acknowledges a delivery.

## Verification

The ordinary suite covers malformed message fields and Reactor notification
routing. Database integration tests cover two-way private chat, multiple online
and offline recipients, sender identity, missing recipients, retries, conflicting
retries, and maximum-length strings requiring JSON escaping. Replay tests cover
disconnects before ACK, ownership, repeated ACKs, batch progression, byte limits,
and online messages arriving during replay. Console tests cover deduplication,
automatic ACKs alongside terminal requests, and EOF behavior.

```bash
./build/tests/chatroom_tests "[message]"
CHATROOM_DB_NAME=chatroom_test ./build/tests/chatroom_database_tests "[message]"
```

Enable `BUILD_DATABASE_TESTS` and supply `CHATROOM_DB_PASSWORD` as described in
[Database](database.md#integration-tests). Use only the disposable test database.
