# Group Chat

Groups use the authenticated TCP connection and the existing persistence, ACK,
and offline-replay pipeline. Membership is chosen at creation. The owner is
always a member; a group may contain at most 64 users.

## Console Commands

After two users log in, use their actual IDs:

```text
/create_group "Study group" <bob-id>
/groups
/members <group-id>
/send_group <group-id> Hello everyone
```

The creator receives the group ID. Other online members receive a creation
notice. Offline members can discover the group later with `/groups`, even if
no messages have been sent. Group names need not be unique.

Use `/create_group Notes` for an owner-only group. Add several users at creation
with comma-separated IDs. Invitations, leaving, removing members, and additional
administrator permissions are not implemented.

## TCP Operations

All operations require an unexpired session. The following examples show request
`data` and response `data`; use the envelope in [Protocol](protocol.md).

### create_group

```json
{"name":"Study group","members":["2","3"]}
```

`name` contains 1–128 UTF-8 bytes and is stored as supplied. `members` is
optional and accepts 0–64 user ID strings before deduplication. The creator is
added automatically; the final set must contain at most 64 users. All must exist.
Supplied ownership fields do not override the session.

Creation saves the group and memberships atomically. Success returns
`{"group_id":"42"}`; a missing user returns `invalid_recipient` without
creating or notifying anything. Each successful creation makes a new group.

After commit, other online members receive:

```json
{"type":"notification","action":"group_created","data":{"group_id":"42","owner_id":"1","name":"Study group","members":["1","2","3"]}}
```

Member IDs are sorted. This is a live notice without a message ID, ACK, or offline
replay. `list_groups` provides discovery after a missed notice.

### list_groups

The first request uses `{}`. Success returns:

```json
{"groups":[{"group_id":"42","owner_id":"1","name":"Study group"}],"next_after_group_id":null}
```

Only the session user's current groups are included, ordered by ascending ID.
Each page has at most 64 entries. The query reads up to 65 rows to detect another
page, without fetching members or user records for each group.

When more results exist, `next_after_group_id` is the last returned ID.
Pass it as `{"after_group_id":"42"}` for the next page. The cursor is exclusive
and uses the standard positive unsigned 64-bit decimal string format.
Malformed cursors receive `invalid_request`.

An empty page returns `{"groups":[],"next_after_group_id":null}`. Each query
reads current memberships; pages do not form a database snapshot.

The console command is `/groups [after-group-id]`. It prints each group's ID,
name, and owner, or `No groups.`. If another page exists, it prints the next
`/groups` command. Run `/groups` without a cursor to refresh from the start.

### get_group_members

Request `{"group_id":"42"}`; success returns
`{"group_id":"42","members":["1","2","3"]}`, with members sorted by ID.

Only current members can query the list. A missing group and a group the caller
does not belong to both return `not_group_member`.

### send_group_message

```json
{"client_message_id":"alice-group-1","group_id":"42","text":"Hello everyone"}
```

The key contains 1–64 bytes and text contains 1–4096 UTF-8 bytes. The sender
comes from the session. Supplied sender or recipient fields cannot change the
broadcast.

Membership is checked inside the transaction. Nonmembers and missing groups
return `not_group_member` without saving anything. Success returns
`{"message_id":"105","group_id":"42"}`.

Each member except the sender gets a delivery and this notification:

```json
{"type":"notification","action":"message","data":{"message_id":"105","group_id":"42","sender_id":"1","client_message_id":"alice-group-1","text":"Hello everyone"}}
```

An owner-only group can save a message with no deliveries. Online recipients get
the notification immediately; offline recipients get it after authentication.
All recipients ACK the same message ID. See [Messaging](messaging.md).

## Transactions and Retries

A new send locks a membership snapshot and saves the message and its deliveries
atomically. The worker returns the persisted recipient IDs; the Reactor resolves
their current connections after commit.

Exact retries reuse the sender's key, group ID, and text. They return the original
message ID and recipient snapshot, without rebuilding deliveries from current
membership. Newly added users therefore do not receive an old message's retry,
and existing delivery records remain valid if membership changes. The sender
must still be a current member.

Private and group messages share a key namespace per sender. Reusing a key for
different text, another group, or another message kind returns
`message_conflict`. Already acknowledged rows remain acknowledged, although
online duplicate notifications may still be sent.
