---
paths:
  - "apps/server/**"
  - "apps/netsim/**"
  - "engine/net/**"
  - "services/lobby/**"
  - "deploy/**"
  - "tests/netcode/**"
  - "tests/unit/net/**"
  - "tests/unit/server/**"
  - "apps/client/core/net_session.*"
  - "docs/lobby-protocol.md"
---

# Server, room and lobby rules

These add to the netcode rules in `CLAUDE.md` ("Netcode (§6)"), which always apply.

- The room: room state never enters `WorldState` or `NetServer` (which only reports
  `RoomSettings`/`Start` events and sends the `RoomState` the host sets); policy is
  `apps/server/room.hpp` driven by `ServerHost`. Waiting and Over mean the host does not call
  `NetServer::tick()` at all (the sim leaves `MatchPhase::Lobby` on its first step); clients
  hold (`NetClient::holding()`) while their `RoomState` is not Playing. Tests and `kfm_netsim`
  that drive `NetServer` directly, and `HostConfig{}` (room off), keep the back-to-back flow.
  A full room queues a joiner as a spectator until a bot yields its slot (round end, or a bot
  off the field).
- Lobby protocol changes go to `docs/lobby-protocol.md` first; the Rust suite, `kfm_lobby_tests`
  (label `network`, a fake lobby in-process) and the client's session test all pin it. Every
  envelope is a flat JSON object of strings (nested signaling rides as a string in `msg`) so
  the engine's hand-written decoder reads it. `ServerHost` must stay lobby-agnostic: the wiring
  is `LobbyAdapter` in apps/server. Additions stay backward compatible (a new key beside the
  old ones; a reader treats a missing key as the older lobby's meaning, as a missing `free`
  means "every listed region free"); `/healthz`'s shape is unchanged.
- A running match does not depend on the lobby (a server keeps its peers and room across a
  lost link and claims the room back at `register`; a client with open data channels ignores
  its lobby socket closing). The lobby never emits a frame over 64 KiB; per-address limits
  refuse `limited`; the units pass the lobby secret through the environment.
