# Protocol (short)

All messages are sent over TCP as text lines and must end with `\n`.

## Connect (client → server)
- `C45<name>\n` — first handshake
- `C45RECONNECT <name> <lobby>\n` — reconnect to running game

## Lobby (client → server)
- `C45<name><lobby>\n` — join lobby
- `C45<name>back\n` — back to lobby list (request snapshot)
- `C45YES\n` — answer for `C45WAITING`

## Game (client → server)
- `C45HIT\n`
- `C45STAND\n`
- `C45PONG\n` — answer for `C45PING`

## Basic server responses
- `C45OK\n` — everything is ok
- `C45WRONG...\n` — protocol error / invalid request
