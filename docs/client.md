# Command-Line Client

The console provides chat commands and readable output. HTTP handles registration,
login, and files; TCP handles authentication, messaging, ACKs, and heartbeats.
The [README](../README.md#verify) walks through two clients.

## Start

```bash
./build/client/chatroom_client
```

Usage:

```text
chatroom_client [options] [IPv4 address] [TCP port] [HTTP port]
```

Defaults are `127.0.0.1`, TCP `8081`, and HTTP `8080`. Addresses must be
numeric IPv4 addresses; ports must be 1–65535. Use `--help` for usage.

| Option | Effect |
| --- | --- |
| `--json` | Display raw incoming TCP JSON |
| `--heartbeat-interval N` | Seconds between automatic probes; default 30 |
| `--heartbeat-timeout N` | Seconds to wait for a matching pong; default 60 |

Both heartbeat settings accept 1–3600 seconds. Keep the interval shorter than
the server's idle timeout. See [Keepalive](keepalive.md).

## Commands

Replace the argument names below with actual values.

| Command | Behavior |
| --- | --- |
| `/register <username> <password>` | Create an account and print its ID; does not log in |
| `/login <username> <password>` | Obtain a JWT over HTTP and authenticate TCP |
| `/send <id[,id...]> <text>` | Send a private or multi-recipient message |
| `/create_group <name> [id[,id...]]` | Create a group containing its creator and selected users |
| `/groups [after-group-id]` | List one page of your groups |
| `/members <group-id>` | List members of a group you belong to |
| `/send_group <group-id> <text>` | Message every other group member |
| `/upload <id[,id...]> <path>` | Upload a file for selected users |
| `/download <file-id> <path>` | Download to a new local file |
| `/ping` | Request a visible pong |
| `/help` | Show commands |
| `/quit` | Cancel I/O and exit |

Wait for `Authenticated as user ...` after login before using authenticated
commands. One connection cannot switch accounts; reconnect first. Logging in
again as the same user refreshes the session. Failed refreshes and rejected
account switches preserve the previous session and HTTP token.

The HTTP file token is retained in memory only after the matching TCP
authentication succeeds. It is never printed or saved. Raw JSON authentication
does not configure the HTTP file client.

## Input and Output

Credentials, group names, and paths support double quotes and backslash escapes:

```text
/register "Alice Smith" "a password with spaces"
/create_group "Study group" 2,3
/upload 2 "./notes final.pdf"
/download 42 "./received notes.pdf"
```

These IDs are examples; use IDs printed by your server. Message text is the
remainder of the line after the command arguments. Quotes in message text are
literal. Leading separator whitespace is skipped; trailing text is preserved.
Passwords are ordinary terminal input and remain visible while typing.

Each input line is limited to 64 KiB, excluding its newline and an optional
trailing carriage return. An oversized line is rejected and discarded through
the next newline; later valid lines can still be read.

The client prints message IDs, senders, group IDs, and file details, escaping
terminal control characters. It remembers the latest 1024 distinct message IDs
to suppress duplicate display and ACKs every copy. This history is in memory
only. Each `/send` or `/send_group` creates a new retry key; repeating the
command sends a new message. There is no automatic uncertain-send retry.

Raw JSON requests are also accepted as individual input lines. The client adds
the TCP length prefix; no manual framing is needed. `--json` displays incoming
payloads, including automatic ACK and heartbeat responses. Registration and
other local command output remain readable text.

## Files and Connection Lifecycle

Downloads never overwrite an existing path. The parent directory must exist;
failed downloads remove partial output. Choose the destination yourself rather
than relying on the server's original filename. File transfers stream in 8 KiB
chunks. See [Files](files.md) for permissions and limits.

The input thread runs commands, a receiver displays TCP traffic and sends ACKs,
and a timer sends heartbeats and reports login expiration. A send mutex serializes
manual requests, ACKs, heartbeats, and the EOF half-close. TCP disconnection also
cancels in-flight HTTP calls.

HTTP authentication uses five-second connect/read/write timeouts and a
ten-second total timeout. File requests use a five-second connect timeout,
30-second read/write timeouts, and a 120-second total timeout. Authentication
replies, upload replies, and download error bodies are limited to 8 KiB;
downloaded bytes stream directly to disk. TCP writes have a five-second socket
timeout.

`/quit` cancels pending I/O immediately. Ctrl+D closes the sending direction
and waits for final replies; it stops heartbeats and automatic ACKs, so keep stdin
open while receiving offline replay. Server disconnection exits even when stdin
is idle and prints a login hint. There is no automatic reconnection.

See [Testing](testing.md#standalone-client) to build the client independently.
