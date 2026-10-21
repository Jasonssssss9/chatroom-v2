# Group Chat

Group chat uses the existing authenticated TCP connection, worker pool, MySQL
transactions, and message-delivery pipeline. Group messages are persisted before
online broadcast and use the same ACK and offline replay as private messages.

## Try It

Register and log in as Alice and Bob in two clients as described in
[Command-Line Client](client.md). Assume Alice's user ID is 1 and Bob's is 2;
replace these with the actual IDs printed during registration.

In Alice's terminal:

```text
/create_group "Study group" 2
```

Alice sees `Created group 42.` Bob sees
`Added to group 42: Study group (owner 1).` Use the actual group ID:

```text
/members 42
/send_group 42 Hello everyone
```

The members response is `Group 42 members: 1, 2.` Alice sees a persistence receipt;
Bob sees the message and automatically acknowledges it:

```text
[group 42, message 105] user 1: Hello everyone
```

Bob can reply with `/send_group 42 Hi Alice`. The sender receives a success
response; every other member has a delivery. The sender does not receive a
second copy of their own group message.

Add more users at creation with `/create_group "Study group" 2,3`, or create an
owner-only group with `/create_group Notes`. Membership is chosen at creation.
Keep the group ID: names need not be unique. Online members receive the creation
notice, and offline members can obtain the ID from a later group message or the
creator. Group creation notices are live events, rather than persisted message
deliveries, and do not have ACKs or offline replay.

## TCP Operations

### create_group

```json
{"type":"request","request_id":"g1","action":"create_group","data":{"name":"Study group","members":["2","3"]}}
```

`name` contains 1 to 128 UTF-8 bytes and is stored as supplied. `members` is an
optional array of 0 to 64 user ID strings before deduplication. Omitting it or
sending an empty array creates an owner-only group. The authenticated creator
is always included; duplicate IDs are merged. The resulting group may contain
at most 64 members, including its owner. Every member must exist.

Success returns `{"group_id":"42"}` in response data. Creation saves the group
and all memberships atomically. Missing users receive `invalid_recipient` without
creating a group or notifying anyone. A supplied `owner_id` is ignored.
Each successful call creates a new group; creation is not an idempotent send.

Other members with a current online session receive:

```json
{"type":"notification","action":"group_created","data":{"group_id":"42","owner_id":"1","name":"Study group","members":["1","2","3"]}}
```

Member IDs are sorted. The creator receives the response instead of this notice.
The notification is sent only after the transaction commits.

### get_group_members

```json
{"type":"request","request_id":"g2","action":"get_group_members","data":{"group_id":"42"}}
```

Success returns `{"group_id":"42","members":["1","2","3"]}` in response data.
Only current members can query the list. A missing group and a group the user
does not belong to both receive `not_group_member`. Member IDs are returned in
ascending order; passwords and user records are not queried.

### send_group_message

```json
{"type":"request","request_id":"g3","action":"send_group_message","data":{"client_message_id":"alice-group-1","group_id":"42","text":"Hello everyone"}}
```

`client_message_id` contains 1 to 64 bytes and `text` contains 1 to 4096 bytes,
using the same rules as private messages. `group_id` is a required positive
unsigned 64-bit decimal string without a sign or leading zeros. All other ID
fields use this same format.

The database checks membership inside the transaction. A missing group or a
sender who is not a current member receives `not_group_member`, with no message
or deliveries created. The sender comes from the session; supplied `sender_id`
or `recipients` fields do not control ownership or broadcast.

Success returns `{"message_id":"105","group_id":"42"}` in response data. Each
recipient receives a normal message notification with the additional group ID:

```json
{"type":"notification","action":"message","data":{"message_id":"105","group_id":"42","sender_id":"1","client_message_id":"alice-group-1","text":"Hello everyone"}}
```

An owner-only group can save a message with no recipient deliveries. Other groups
save one delivery for each member except the sender. Online users receive the
notification; offline users receive it after authentication. All recipients use
`ack_message` with the same message ID. The existing replay window, byte limits,
duplicate handling, and shutdown behavior apply unchanged. See [Messaging](messaging.md).

## Transactions and Retries

For a new send, `ChatStore::save_group_message()` locks a membership snapshot,
checks the sender, and saves the message and deliveries in one transaction.
It returns both the message ID and its persisted recipient IDs. The worker
returns those IDs with the serialized notification; the Reactor resolves their
current connections after commit.

For an exact retry, reuse the sender's client message ID, group ID, and text.
The store returns the original message and its original delivery snapshot.
It does not rebuild broadcast recipients from the current membership list.
Thus members added later do not receive the old message's retry, and an original
recipient's delivery remains valid if membership later changes. Already
acknowledged deliveries remain acknowledged, though an online duplicate notice
can still be sent and acknowledged again. The sender must still be a current
member when retrying.

Client message IDs share one namespace per sender across private and group
messages. Reusing one with different text, another group, or a different message
kind receives `message_conflict`. Group creation does not use this message key.

No schema changes are needed: the existing group, membership, message, and
delivery tables hold this data. All SQL and serialization run on workers, while
socket access and online routing remain on the Reactor.

## Verification

Tests cover creation, missing users, owner-only groups, the 64-member limit,
permission checks, two-way group broadcast, ignored ownership/recipient fields,
offline replay, ACKs, conflicts, and retries after membership changes.
Console tests cover all three commands, group notices, display, deduplication,
and automatic ACKs.

```bash
./build/tests/chatroom_tests "[group]"
CHATROOM_DB_NAME=chatroom_test ./build/tests/chatroom_database_tests "[group]"
```

Enable `BUILD_DATABASE_TESTS` and supply the password as described in
[Database](database.md#integration-tests). Use only the disposable test database.
