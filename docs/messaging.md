# Private and Multi-Recipient Messages

The server supports persisted text messages between authenticated TCP clients.
Private messages and multi-recipient sends use the same `send_message` action.
The command-line client already displays both responses and notifications as JSON.

## Try Two Clients

Start the server with MySQL and JWT environment variables configured as described
in [Authentication](authentication.md). Its default HTTP and TCP ports are 8080
and 8081.

Register `alice` and `bob` through `POST /api/users`, then log in through
`POST /api/auth/token`. Follow the curl examples in the authentication guide.
Keep each account's JWT and user ID from its registration response. The IDs below
are examples; replace them with the actual IDs in your database.

In Alice's terminal, start the client:

```bash
./build/client/chatroom_client 127.0.0.1 8081
```

Authenticate with Alice's token:

```json
{"type":"request","request_id":"alice-auth","action":"authenticate","data":{"token":"<ALICE_JWT>"}}
```

Start a second client in Bob's terminal and authenticate with Bob's token:

```json
{"type":"request","request_id":"bob-auth","action":"authenticate","data":{"token":"<BOB_JWT>"}}
```

After both authentication responses succeed, Alice can send to Bob. This example
assumes Alice's ID is `"1"` and Bob's is `"2"`:

```json
{"type":"request","request_id":"send-1","action":"send_message","data":{"client_message_id":"alice-1","recipients":["2"],"text":"Hello Bob"}}
```

Alice receives a response containing the persisted message ID:

```json
{"type":"response","request_id":"send-1","action":"send_message","ok":true,"data":{"message_id":"105"}}
```

Bob receives a notification without sending a request:

```json
{"type":"notification","action":"message","data":{"message_id":"105","sender_id":"1","client_message_id":"alice-1","text":"Hello Bob"}}
```

Bob can reply to Alice with his own client message ID:

```json
{"type":"request","request_id":"reply-1","action":"send_message","data":{"client_message_id":"bob-1","recipients":["1"],"text":"Hi Alice"}}
```

Use `/quit` to exit. Authenticate again after reconnecting. If a token expires,
log in through HTTP again and send `authenticate` with the new token.

## Multiple Recipients and Retries

Add user IDs to the same recipient array:

```json
{"type":"request","request_id":"send-2","action":"send_message","data":{"client_message_id":"alice-2","recipients":["2","3"],"text":"Hello everyone"}}
```

Both users must exist. They share one stored message and message ID, with a
delivery row for each recipient. Repeated IDs are merged. You may include your
own ID. Online recipients get notifications; offline deliveries stay in MySQL.
See [JSON Protocol](protocol.md#send_message) for field limits and error codes.

Choose a new `client_message_id` for each new outgoing message. For an uncertain
send result, retry with the same client ID, text, and recipient set. The response
uses the existing message ID; it does not create another message or delivery.
Notifications can repeat, so a client that maintains a message history should
deduplicate by `message_id`. The current console client displays received JSON
directly and does not maintain that history.

## Persistence and Thread Ownership

1. A TCP worker reads the sender from its session snapshot and validates the data.
2. It checks recipients and commits the message and deliveries in MySQL.
3. It returns the response and one serialized notification with recipient user IDs.
4. The Reactor resolves each recipient's current, unexpired session and queues output.

Workers do not access sockets or online maps. A disconnected sender's late
response is discarded, while its committed message can still notify recipients.
Changing the recipient's connection during database work routes the notification
to the new connection. A failed or oversized result is not routed.

Online notification does not acknowledge a delivery. This step does not implement
network ACKs, automatic offline replay, or background retries. Those will use the
existing pending-message and acknowledgment database operations. Until then,
pending rows remain even after successful online notification.

## Verification

The ordinary suite covers malformed message fields and Reactor notification
routing. Database integration tests cover two-way private chat, multiple online
and offline recipients, sender identity, missing recipients, retries, conflicting
retries, and maximum-length strings requiring JSON escaping.

```bash
./build/tests/chatroom_tests "[message]"
CHATROOM_DB_NAME=chatroom_test ./build/tests/chatroom_database_tests "[message]"
```

Enable `BUILD_DATABASE_TESTS` and supply `CHATROOM_DB_PASSWORD` as described in
[Database](database.md#integration-tests). Use only the disposable test database.
