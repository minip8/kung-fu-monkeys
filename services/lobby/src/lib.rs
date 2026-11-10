//! The Kung Fu Monkeys lobby service (architecture v0.37 §13.5): room codes,
//! connection tokens, the game server registry and the WebRTC signaling
//! relay, all in memory. The wire format is docs/lobby-protocol.md.
//!
//! The binary is a thin CLI over [`serve`]; tests bind a listener on port 0
//! and pass it in with a [`LobbyConfig`] whose timers are short.

mod connection;
mod limits;
mod lobby;
pub mod protocol;

use std::future::Future;
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;

use axum::extract::ws::WebSocketUpgrade;
use axum::extract::{ConnectInfo, State};
use axum::http::HeaderMap;
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use axum::{Json, Router};
use tokio::net::TcpListener;

use crate::lobby::Lobby;

/// Settings of one lobby instance. [`LobbyConfig::new`] gives the values of
/// docs/lobby-protocol.md; the `with_*` methods override them (tests shorten
/// the timers).
#[derive(Debug, Clone)]
pub struct LobbyConfig {
    /// Shared secret every game server presents in `register`. Never empty.
    pub secret: String,
    /// How long a room with no client sockets lives (§6.3; 600 s).
    pub room_ttl: Duration,
    /// How long a socket may wait before its first message (§7; 10 s).
    pub first_message_timeout: Duration,
    /// Sustained messages per second per socket (§7; 60).
    pub rate_per_second: f64,
    /// Burst of messages per socket (§7; 120).
    pub rate_burst: f64,
    /// Largest text frame (§1; 64 KiB).
    pub max_frame_bytes: usize,
    /// Open client sockets per room (§7; 64).
    pub max_clients_per_room: usize,
    /// Registered servers (§7; 1024).
    pub max_servers: usize,
    /// Live rooms (§7; 4096).
    pub max_rooms: usize,
    /// Distinct regions among the registered servers (§7; 64). Bounds the
    /// `regions` reply, which lists them twice, well under `max_frame_bytes`.
    pub max_regions: usize,
    /// Proxies whose `X-Forwarded-For` names the client (§7, v1.3; none:
    /// every client is its TCP peer address). Caddy on the lobby VM is
    /// `127.0.0.1`.
    pub trusted_proxies: Vec<IpAddr>,
    /// Open `/client` sockets per client address (§7; 32).
    pub max_connections_per_address: usize,
    /// Live rooms one client address may have created (§7; 4).
    pub max_rooms_per_address: usize,
    /// `create` and `join` requests per client address: sustained per
    /// minute (§7; 30) and burst (30).
    pub requests_per_minute: f64,
    pub requests_burst: f64,
    /// Joins refused `unknown_code` or `bad_token` per client address:
    /// sustained per minute (§7; 1) and burst (10).
    pub failed_joins_per_minute: f64,
    pub failed_joins_burst: f64,
    /// Client addresses tracked at once (§7; 65536); past it, idle ones are
    /// forgotten and a new address is refused `limited` if none was idle.
    pub max_tracked_addresses: usize,
    /// Tokens a room remembers for rejoins (§2; 128, at least
    /// `max_clients_per_room`): the oldest unseated one makes way.
    pub max_tokens_per_room: usize,
    /// How long a room no game connection has reached is held (§6; 60 s):
    /// from its creation with sockets open, and after its last socket.
    pub unused_room_ttl: Duration,
}

impl LobbyConfig {
    /// The protocol's defaults with this shared secret.
    pub fn new(secret: impl Into<String>) -> Self {
        LobbyConfig {
            secret: secret.into(),
            room_ttl: Duration::from_secs(600),
            first_message_timeout: Duration::from_secs(10),
            rate_per_second: 60.0,
            rate_burst: 120.0,
            max_frame_bytes: protocol::MAX_FRAME_BYTES,
            max_clients_per_room: 64,
            max_servers: 1024,
            max_rooms: 4096,
            max_regions: 64,
            trusted_proxies: Vec::new(),
            max_connections_per_address: 32,
            max_rooms_per_address: 4,
            requests_per_minute: 30.0,
            requests_burst: 30.0,
            failed_joins_per_minute: 1.0,
            failed_joins_burst: 10.0,
            max_tracked_addresses: 65536,
            max_tokens_per_room: 128,
            unused_room_ttl: Duration::from_secs(60),
        }
    }

    pub fn with_room_ttl(mut self, ttl: Duration) -> Self {
        self.room_ttl = ttl;
        self
    }

    pub fn with_first_message_timeout(mut self, timeout: Duration) -> Self {
        self.first_message_timeout = timeout;
        self
    }

    pub fn with_rate_limit(mut self, per_second: f64, burst: f64) -> Self {
        self.rate_per_second = per_second;
        self.rate_burst = burst;
        self
    }

    pub fn with_trusted_proxies(mut self, proxies: Vec<IpAddr>) -> Self {
        self.trusted_proxies = proxies;
        self
    }

    pub fn with_unused_room_ttl(mut self, ttl: Duration) -> Self {
        self.unused_room_ttl = ttl;
        self
    }
}

/// The lobby's routes: `/client`, `/server` (WebSockets) and `/healthz`.
pub fn router(mut config: LobbyConfig) -> Router {
    // No setting may make a reply outgrow what its receiver accepts (§7).
    config.max_regions = config
        .max_regions
        .min(protocol::max_regions_in_a_frame(config.max_frame_bytes));
    // Every seated socket holds a token, so the table never fills with them.
    config.max_tokens_per_room = config.max_tokens_per_room.max(config.max_clients_per_room);
    let lobby = Lobby::new(config);
    Router::new()
        .route("/client", get(client_socket))
        .route("/server", get(server_socket))
        .route("/healthz", get(healthz))
        .with_state(lobby)
}

/// Serves the lobby on `listener` until `shutdown` resolves. The routes need
/// each connection's peer address (the per-address limits), so serve them
/// with `into_make_service_with_connect_info::<SocketAddr>()` as here.
pub async fn serve(
    config: LobbyConfig,
    listener: TcpListener,
    shutdown: impl Future<Output = ()> + Send + 'static,
) -> std::io::Result<()> {
    axum::serve(
        listener,
        router(config).into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown)
    .await
}

/// Serves the lobby on `listener` forever.
pub async fn run(config: LobbyConfig, listener: TcpListener) -> std::io::Result<()> {
    serve(config, listener, std::future::pending()).await
}

/// Bytes tungstenite buffers before giving up on a message. Larger than the
/// protocol limit so an oversized frame reaches our own check and gets a
/// clean 1002 close instead of a dropped connection.
const TRANSPORT_MESSAGE_LIMIT: usize = 1024 * 1024;

fn upgrade(ws: WebSocketUpgrade) -> WebSocketUpgrade {
    ws.max_message_size(TRANSPORT_MESSAGE_LIMIT)
        .max_frame_size(TRANSPORT_MESSAGE_LIMIT)
}

async fn client_socket(
    State(lobby): State<Arc<Lobby>>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    ws: WebSocketUpgrade,
) -> Response {
    let addr = limits::client_address(peer, &headers, &lobby.config.trusted_proxies);
    upgrade(ws).on_upgrade(move |socket| connection::client(lobby, socket, addr))
}

async fn server_socket(State(lobby): State<Arc<Lobby>>, ws: WebSocketUpgrade) -> Response {
    upgrade(ws).on_upgrade(move |socket| connection::server(lobby, socket))
}

async fn healthz(State(lobby): State<Arc<Lobby>>) -> impl IntoResponse {
    let health = lobby.health();
    Json(serde_json::json!({
        "servers": health.servers,
        "rooms": health.rooms,
        "regions": health.regions,
    }))
}
