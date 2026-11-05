//! `kfm-lobby`: the lobby service binary (docs/lobby-protocol.md §8).

use std::net::SocketAddr;
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
    let config = LobbyConfig::new(secret)
        .with_room_ttl(ttl)
        .with_first_message_timeout(Duration::from_millis(args.first_message_timeout_ms));

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
