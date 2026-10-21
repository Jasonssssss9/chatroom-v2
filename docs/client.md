# Command-Line Client

The client provides commands for account registration, login, and chat. HTTP
handles account operations; login automatically sends the returned JWT through
TCP authentication. The JWT stays in memory and is not displayed or saved.

## Start the Client

```bash
./build/client/chatroom_client
./build/client/chatroom_client 127.0.0.1 8081 8080
./build/client/chatroom_client --json 127.0.0.1 8081 8080
./build/client/chatroom_client --help
```

Arguments are the numeric IPv4 address, TCP port, and HTTP port, in that order.
Defaults are `127.0.0.1`, `8081`, and `8080`. Both ports must be between 1 and
65535. HTTP and TCP use the same server address. The server must be running;
the client opens its TCP connection at startup. The client requires no database
credentials or JWT signing secret.

## Commands

| Command | Purpose |
| --- | --- |
| `/register <username> <password>` | Create an account and display its user ID |
| `/login <username> <password>` | Log in over HTTP and authenticate this TCP connection |
| `/send <id[,id...]> <text>` | Send a private or multi-recipient message |
| `/ping` | Check the TCP connection |
| `/help` | List commands |
| `/quit` | Disconnect and exit |

For example, in Alice's terminal:

```text
/register alice example-password
/login alice example-password
```

Registration prints `Registered user 1. Use /login to sign in.` The exact ID
depends on the database. Registration does not log in automatically. Login
prints `Authenticated as user 1.` only after TCP authentication succeeds.

Register and log in as Bob in a second terminal, note his ID, and send to it:

```text
/send 2 Hello Bob
/send 1,2 Hello everyone
```

The sender sees `Sent message 105.` after persistence. The recipient sees:

```text
[message 105] user 1: Hello Bob
```

Use actual registration IDs rather than assuming they are 1 and 2. Recipient
IDs are positive decimal strings without signs or leading zeros. Use commas
without spaces between IDs; there may be 1 to 64 entries. The server merges
duplicates. Message text is the rest of the line after separating whitespace,
including any trailing spaces. Quotes in message text are literal. Text must
contain 1 to 4096 UTF-8 bytes.

Quote a username or password containing spaces:

```text
/register "alice smith" "example password"
/login "alice smith" "example password"
```

Inside quoted credentials, escape a quote as `\"` and a backslash as `\\`.
Commands are read directly from stdin; the terminal echoes credentials as typed.
Usernames contain 1 to 64 bytes and passwords 8 to 128 bytes, matching the API.

Each `/send` generates a fresh `client_message_id` from a random per-process
prefix and a sequence number. Restarting the client does not reuse a fixed
counter as a message ID. The client does not automatically retry uncertain sends;
raw JSON requests can reuse a known client message ID for an exact retry.

## Messages, Errors, and Reconnection

The receiver continues processing messages and automatic ACKs during HTTP
registration or login. TCP writes are serialized with terminal requests.
Successful automatic ACK responses are hidden in normal output; failures remain
visible. Recent duplicate messages are displayed once but acknowledged every
time. The history holds the most recent 1024 distinct IDs in memory.

Errors identify the operation and the server's error code, for example:

```text
Error [send_message]: authentication_required: Authenticate before using this action
Error: invalid_credentials: Invalid username or password
```

Command syntax errors and HTTP failures leave the TCP connection available for
later commands. HTTP has five-second connection/read/write timeouts and a
ten-second total request limit. Unexpected TCP failures end the client.
Message text and errors escape control characters so a newline or escape byte
cannot overwrite terminal output. UTF-8 text remains readable.

If a token expires, use `/login` again for the same user. To switch accounts,
quit and reconnect; the server does not permit changing users on one connection.
A second login on another connection replaces the first one. There is no
automatic reconnection. See [Authentication](authentication.md).

Use `/quit` for immediate cancellation. Stdin EOF sends FIN and waits for final
server output. After that half-close the client displays notifications without
ACKs, so keep stdin open to finish automatic replay. Input is still bounded to
64 KiB per line; an oversized line is discarded through its newline.

## JSON Debugging

JSON requests remain accepted in either output mode. Use `--json` to display raw
TCP payloads with the existing `<` prefix, including automatic ACK responses:

```bash
printf '%s\n' '{"type":"request","request_id":"p1","action":"ping"}' | \
    ./build/client/chatroom_client --json
```

The flag changes display only. Commands, automatic TCP authentication, message
deduplication, and ACKs keep the same behavior. HTTP login responses and tokens
are never printed. See [JSON Protocol](protocol.md) for raw request examples.
