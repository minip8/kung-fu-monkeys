//! `kfm-lobby`: the lobby service binary (docs/lobby-protocol.md §8).

use std::net::{IpAddr, SocketAddr};
use std::process::ExitCode;
use std::time::Duration;

use clap::Parser;
use kfm_lobby::LobbyConfig;
use tracing_subscriber::EnvFilter;

/// Kung Fu Monkeys lobby: room codes, tokens, the server registry and the
/// WebRTC signaling relay. Caddy terminates TLS in front of --listen.
#[derive(Parser, Debug)]
#[command(name = "kfm-lobby", version)]
struct Args {
    /// Address to listen on (plain HTTP/WebSocket).
    #[arg(long, default_value = "127.0.0.1:8080")]
    listen: SocketAddr,

    /// Shared secret game servers present in `register`. Required, never empty.
    #[arg(long, env = "KFM_LOBBY_SECRET", hide_env_values = true)]
    secret: Option<String>,

    /// Seconds a room with no clients lives before it closes.
    #[arg(long, default_value_t = 600)]
    room_ttl_seconds: u64,

    /// Room expiry in milliseconds; overrides --room-ttl-seconds (testing).
    #[arg(long, hide = true)]
    room_ttl_ms: Option<u64>,

    /// Milliseconds a socket may wait before its first message (testing).
    #[arg(long, hide = true, default_value_t = 10_000)]
    first_message_timeout_ms: u64,

    /// A proxy (Caddy) whose X-Forwarded-For names the client; repeatable.
    /// Without one every client counts as its TCP peer address, so behind a
    /// proxy all clients would share the proxy's limits.
    #[arg(long = "trusted-proxy", value_name = "IP")]
    trusted_proxies: Vec<IpAddr>,

    /// Open client sockets per client address.
    #[arg(long, default_value_t = 32)]
    max_connections_per_address: usize,

    /// Live rooms one client address may have created.
    #[arg(long, default_value_t = 4)]
    max_rooms_per_address: usize,

    /// create and join requests per client address and minute (also the burst).
    #[arg(long, default_value_t = 30.0)]
    requests_per_minute: f64,

    /// Joins refused unknown_code or bad_token per client address: the burst...
    #[arg(long, default_value_t = 10.0)]
    failed_joins_burst: f64,

    /// ... and the sustained rate per minute after it.
    #[arg(long, default_value_t = 1.0)]
    failed_joins_per_minute: f64,

    /// Tokens a room keeps for rejoins (the oldest unseated one makes way).
    #[arg(long, default_value_t = 128)]
    max_tokens_per_room: usize,

    /// Seconds a room that no game connection reached is held.
    #[arg(long, default_value_t = 60)]
    unused_room_ttl_seconds: u64,

    /// The unused-room hold in milliseconds; overrides the seconds (testing).
    #[arg(long, hide = true)]
    unused_room_ttl_ms: Option<u64>,

    /// Log filter: a level (error, warn, info, debug, trace) or a tracing directive.
    #[arg(long, default_value = "info")]
    log_level: String,
}

fn main() -> ExitCode {
    let args = Args::parse();

    let filter = match EnvFilter::try_new(&args.log_level) {
        Ok(filter) => filter,
        Err(err) => {
            eprintln!("kfm-lobby: bad --log-level {:?}: {err}", args.log_level);
            return ExitCode::from(2);
        }
    };
    tracing_subscriber::fmt().with_env_filter(filter).init();

    let secret = args.secret.unwrap_or_default();
    if secret.is_empty() {
        eprintln!("kfm-lobby: --secret (or KFM_LOBBY_SECRET) is required and must not be empty");
        return ExitCode::from(2);
    }

    // Sentry starts before the runtime so it sees every thread.
    #[cfg(feature = "sentry")]
    let _sentry = std::env::var("SENTRY_DSN")
        .ok()
        .filter(|dsn| !dsn.is_empty())
        .map(|dsn| {
            let mut options = sentry::ClientOptions::default();
            options.release = sentry::release_name!();
            sentry::init((dsn, options))
        });

    let ttl = match args.room_ttl_ms {
        Some(ms) => Duration::from_millis(ms),
        None => Duration::from_secs(args.room_ttl_seconds),
    };
    let unused_ttl = match args.unused_room_ttl_ms {
        Some(ms) => Duration::from_millis(ms),
        None => Duration::from_secs(args.unused_room_ttl_seconds),
    };
    let mut config = LobbyConfig::new(secret)
        .with_room_ttl(ttl)
        .with_first_message_timeout(Duration::from_millis(args.first_message_timeout_ms))
        .with_trusted_proxies(args.trusted_proxies)
        .with_unused_room_ttl(unused_ttl);
    config.max_connections_per_address = args.max_connections_per_address;
    config.max_rooms_per_address = args.max_rooms_per_address;
    config.requests_per_minute = args.requests_per_minute;
    config.requests_burst = args.requests_per_minute;
    config.failed_joins_burst = args.failed_joins_burst;
    config.failed_joins_per_minute = args.failed_joins_per_minute;
    config.max_tokens_per_room = args.max_tokens_per_room;
    let proxies = format!("{:?}", config.trusted_proxies);

    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(err) => {
            eprintln!("kfm-lobby: cannot start the runtime: {err}");
            return ExitCode::FAILURE;
        }
    };
    let result = runtime.block_on(async move {
        let listener = tokio::net::TcpListener::bind(args.listen).await?;
        tracing::info!(
            listen = %listener.local_addr()?,
            room_ttl_ms = ttl.as_millis() as u64,
            unused_room_ttl_ms = unused_ttl.as_millis() as u64,
            trusted_proxies = %proxies,
            "kfm-lobby listening"
        );
        kfm_lobby::serve(config, listener, async {
            let _ = tokio::signal::ctrl_c().await;
            tracing::info!("shutting down");
        })
        .await
    });
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("kfm-lobby: {err}");
            ExitCode::FAILURE
        }
    }
}
