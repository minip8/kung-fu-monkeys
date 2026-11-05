# kfm-lobby

The Kung Fu Monkeys lobby service (architecture v0.37 §13.5): the only public TCP endpoint of
an M6a deployment. Game servers (`kfm_server`) dial `/server` and register; browsers and native
clients dial `/client` to list regions, create a room (a five-letter code on a free server) or
join one, and then exchange WebRTC signaling with that room's server over the same socket.
Everything lives in memory: no database, no files. `/healthz` reports registered servers,
live rooms and the regions that can host a room.

The wire format is normative in [`docs/lobby-protocol.md`](../../docs/lobby-protocol.md);
change it there first.

## Run locally

```sh
cd services/lobby
cargo run -- --listen 127.0.0.1:8080 --secret dev
curl http://127.0.0.1:8080/healthz
```

Flags: `--listen ADDR` (default `127.0.0.1:8080`), `--secret S` or `KFM_LOBBY_SECRET`
(required, never empty), `--room-ttl-seconds N` (default 600), `--log-level LEVEL` (default
`info`, any `tracing` filter directive). `SENTRY_DSN` in the environment enables Sentry; build
with `--no-default-features` to leave Sentry out entirely. In production Caddy terminates TLS
in front of `--listen`.

## Test

```sh
cargo fmt --check --manifest-path services/lobby/Cargo.toml
cargo clippy --manifest-path services/lobby/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path services/lobby/Cargo.toml
```

`tests/lobby.rs` starts the lobby in process on an ephemeral port (`kfm_lobby::run` with a
`LobbyConfig` whose timers are short) and drives it over real WebSockets as servers and
clients. The `lobby` job of `.github/workflows/ci.yml` runs the same three commands.
