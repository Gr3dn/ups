# Protocol

All messages are sent over TCP as text lines and must end with `\n`.

## Contents
- Connect
- Lobby
- Game
- Keepalive
- Basic server responses

## Connect (client -> server)
- `C45<name>\n` — first handshake
- `C45REC <name> <lobby>\n` — reconnect/resume session (`lobby=0` means "unknown; resume to lobby list if not in a game")

## Lobby (client -> server)
- `C45J <lobby>\n` — join lobby
- `C45B\n` — back to lobby list / request lobby snapshot

## Game (client -> server)
- `C45H\n` — HIT
- `C45S\n` — STAND
- `C45PO\n` — answer for `C45PI`

## Keepalive (both directions)
- `C45PI\n` — PING
- `C45PO\n` — PONG (answer for `C45PI`)

## Lobby snapshot (server -> client)
- `C45L <n> <pairs>\n` — compact lobby list snapshot
  - `<n>`: lobby count
  - `<pairs>`: 2×`n` digits, each pair is `players` (0..2) + `status` (0/1)
  - Example for 3 lobbies: `C45L 3 001020\n`

## Game (server -> client)
- `C45D <c1> <c2>\n` — initial deal (two cards)
- `C45T <name> <sec>\n` — whose turn, and turn timeout in seconds
- `C45C <card>\n` — a card drawn (e.g. `AS`)
- `C45B <name> <value>\n` — bust (hand value exceeded 21)
- `C45TO\n` — local player timed out (auto-stand)
- `C45R <p1> <s1> <p2> <s2> <winner>\n` — game result (`winner` may be `PUSH`)
- `C45OD <name> <sec>\n` — opponent disconnected (server will wait up to `<sec>` seconds)
- `C45OB <name>\n` — opponent reconnected

## Basic server responses
- `C45OK\n` — everything is ok
- `C45WRONG...\n` — protocol error / invalid request
- `C45REC_OK\n` — reconnect accepted (game will resume or client will continue waiting)
- `C45DOWN [reason]\n` — server is shutting down; client should disconnect
