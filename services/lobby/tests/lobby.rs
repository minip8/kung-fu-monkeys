//! Integration tests: an in-process lobby on an ephemeral port, driven over
//! real WebSockets the way game servers and clients use it
//! (docs/lobby-protocol.md).

use std::collections::HashMap;
use std::net::SocketAddr;
use std::time::Duration;

use futures_util::{SinkExt, StreamExt};
use kfm_lobby::LobbyConfig;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio_tungstenite::tungstenite::protocol::frame::coding::CloseCode;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::{MaybeTlsStream, WebSocketStream};

type Ws = WebSocketStream<MaybeTlsStream<TcpStream>>;
type Fields = HashMap<String, String>;

const SECRET: &str = "test-secret";
const BUILD: &str = "0badc0de";
const CONTENT: &str = "12345678";
const WAIT: Duration = Duration::from_secs(5);

fn config() -> LobbyConfig {
    LobbyConfig::new(SECRET)
}

async fn start(config: LobbyConfig) -> SocketAddr {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(kfm_lobby::run(config, listener));
    addr
}

async fn connect(addr: SocketAddr, path: &str) -> Ws {
    let (ws, _) = tokio_tungstenite::connect_async(format!("ws://{addr}{path}"))
        .await
        .unwrap();
    ws
}

fn json(pairs: &[(&str, &str)]) -> String {
    let map: serde_json::Map<String, serde_json::Value> = pairs
        .iter()
        .map(|(k, v)| ((*k).to_owned(), serde_json::Value::String((*v).to_owned())))
        .collect();
    serde_json::Value::Object(map).to_string()
}

async fn send(ws: &mut Ws, text: impl Into<String>) {
    ws.send(Message::text(text.into())).await.unwrap();
}

/// The next text frame, raw.
async fn recv_text(ws: &mut Ws) -> String {
    loop {
        let message = tokio::time::timeout(WAIT, ws.next())
            .await
            .expect("timed out waiting for a message")
            .expect("socket ended")
            .expect("socket error");
        match message {
            Message::Text(text) => return text.as_str().to_owned(),
            Message::Ping(_) | Message::Pong(_) => continue,
            other => panic!("expected a text frame, got {other:?}"),
        }
    }
}

async fn recv(ws: &mut Ws) -> Fields {
    let text = recv_text(ws).await;
    serde_json::from_str(&text).unwrap_or_else(|e| panic!("not a flat object {text}: {e}"))
}

/// Reads until the close frame and returns its code; `None` if the stream
/// ended without one. Panics on a text frame.
async fn expect_close(ws: &mut Ws) -> Option<u16> {
    loop {
        let next = tokio::time::timeout(WAIT, ws.next())
            .await
            .expect("timed out waiting for the close");
        match next {
            None | Some(Err(_)) => return None,
            Some(Ok(Message::Close(frame))) => return frame.map(|f| u16::from(f.code)),
            Some(Ok(Message::Text(text))) => panic!("expected a close, got {text}"),
            Some(Ok(_)) => continue,
        }
    }
}

/// Asserts nothing arrives for a while.
async fn expect_silence(ws: &mut Ws, for_: Duration) {
    if let Ok(message) = tokio::time::timeout(for_, ws.next()).await {
        panic!("expected silence, got {message:?}");
    }
}

/// A `refused` with this reason, then close 1000.
async fn expect_refused(ws: &mut Ws, reason: &str) {
    let msg = recv(ws).await;
    assert_eq!(msg["type"], "refused", "{msg:?}");
    assert_eq!(msg["reason"], reason, "{msg:?}");
    assert_eq!(expect_close(ws).await, Some(1000));
}

fn register_msg(name: &str, region: &str, capacity: &str) -> String {
    json(&[
        ("type", "register"),
        ("secret", SECRET),
        ("region", region),
        ("name", name),
        ("build", BUILD),
        ("content", CONTENT),
        ("capacity", capacity),
    ])
}

async fn register(addr: SocketAddr, name: &str, region: &str, capacity: &str) -> Ws {
    let mut ws = connect(addr, "/server").await;
    send(&mut ws, register_msg(name, region, capacity)).await;
    let reply = recv(&mut ws).await;
    assert_eq!(reply["type"], "registered", "{reply:?}");
    ws
}

async fn request(addr: SocketAddr, pairs: &[(&str, &str)]) -> (Ws, Fields) {
    let mut ws = connect(addr, "/client").await;
    send(&mut ws, json(pairs)).await;
    let reply = recv(&mut ws).await;
    (ws, reply)
}

async fn create(addr: SocketAddr, region: &str) -> (Ws, Fields) {
    request(
        addr,
        &[
            ("type", "create"),
            ("region", region),
            ("build", BUILD),
            ("content", CONTENT),
        ],
    )
    .await
}

async fn join(addr: SocketAddr, code: &str) -> (Ws, Fields) {
    request(
        addr,
        &[
            ("type", "join"),
            ("code", code),
            ("build", BUILD),
            ("content", CONTENT),
        ],
    )
    .await
}

/// The server side of a `create`: `room` then `peer`. Returns (code, peer, token).
async fn expect_room(server: &mut Ws) -> (String, String, String) {
    let room = recv(server).await;
    assert_eq!(room["type"], "room", "{room:?}");
    let (peer, token) = expect_peer(server).await;
    (room["code"].clone(), peer, token)
}

async fn expect_peer(server: &mut Ws) -> (String, String) {
    let peer = recv(server).await;
    assert_eq!(peer["type"], "peer", "{peer:?}");
    (peer["peer"].clone(), peer["token"].clone())
}

/// The `regions` reply as (list, free): every registered region and the
/// ones with a free server (lobby protocol v1.1).
async fn regions(addr: SocketAddr) -> (String, String) {
    let (mut ws, reply) = request(addr, &[("type", "regions")]).await;
    assert_eq!(reply["type"], "regions");
    assert_eq!(expect_close(&mut ws).await, Some(1000));
    (reply["list"].clone(), reply["free"].clone())
}

fn pair(list: &str, free: &str) -> (String, String) {
    (list.to_owned(), free.to_owned())
}

async fn healthz(addr: SocketAddr) -> serde_json::Value {
    let mut stream = TcpStream::connect(addr).await.unwrap();
    stream
        .write_all(b"GET /healthz HTTP/1.1\r\nHost: lobby\r\nConnection: close\r\n\r\n")
        .await
        .unwrap();
    let mut response = String::new();
    stream.read_to_string(&mut response).await.unwrap();
    assert!(response.starts_with("HTTP/1.1 200"), "{response}");
    let body = response.split("\r\n\r\n").nth(1).unwrap();
    serde_json::from_str(body).unwrap()
}

/// Polls `/healthz` until `check` holds (state changes after a close are
/// asynchronous).
async fn wait_health(addr: SocketAddr, check: impl Fn(&serde_json::Value) -> bool) {
    let deadline = tokio::time::Instant::now() + WAIT;
    loop {
        let health = healthz(addr).await;
        if check(&health) {
            return;
        }
        assert!(
            tokio::time::Instant::now() < deadline,
            "health never matched: {health}"
        );
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

// --- registration ---------------------------------------------------------

#[tokio::test]
async fn register_with_good_and_bad_secret() {
    let addr = start(config()).await;

    let mut bad = connect(addr, "/server").await;
    send(
        &mut bad,
        register_msg("eu-1", "eu", "4").replace(SECRET, "wrong-secret"),
    )
    .await;
    expect_refused(&mut bad, "bad_secret").await;

    let mut missing = connect(addr, "/server").await;
    send(
        &mut missing,
        json(&[("type", "register"), ("secret", SECRET), ("region", "eu")]),
    )
    .await;
    expect_refused(&mut missing, "bad_request").await;

    let mut not_register = connect(addr, "/server").await;
    send(
        &mut not_register,
        json(&[("type", "capacity"), ("capacity", "3")]),
    )
    .await;
    expect_refused(&mut not_register, "bad_request").await;

    let mut bad_capacity = connect(addr, "/server").await;
    send(&mut bad_capacity, register_msg("eu-1", "eu", "lots")).await;
    expect_refused(&mut bad_capacity, "bad_request").await;

    let _good = register(addr, "eu-1", "eu", "4").await;
    let health = healthz(addr).await;
    assert_eq!(health["servers"], 1);
}

#[tokio::test]
async fn a_full_registry_refuses_bad_request() {
    let mut config = config();
    config.max_servers = 2;
    let addr = start(config).await;
    let _a = register(addr, "eu-1", "eu", "4").await;
    let b = register(addr, "eu-2", "eu", "4").await;
    let mut c = connect(addr, "/server").await;
    send(&mut c, register_msg("eu-3", "eu", "4")).await;
    expect_refused(&mut c, "bad_request").await;
    // A slot that comes free takes a registration again.
    drop(b);
    wait_health(addr, |h| h["servers"] == 1).await;
    let _c = register(addr, "eu-3", "eu", "4").await;
}

#[tokio::test]
async fn regions_lists_every_region_and_the_free_ones() {
    let addr = start(config()).await;
    assert_eq!(regions(addr).await, pair("", ""));

    let mut eu = register(addr, "eu-1", "eu", "4").await;
    let _us = register(addr, "us-1", "us", "4").await;
    let _asia_full = register(addr, "asia-1", "asia", "0").await;
    // asia has a server but no human slot: listed, not free.
    assert_eq!(regions(addr).await, pair("asia,eu,us", "eu,us"));

    // The only eu server now hosts a room: eu stays listed but is busy.
    let (_client, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut eu).await;
    assert_eq!(regions(addr).await, pair("asia,eu,us", "us"));
}

#[tokio::test]
async fn create_in_a_busy_listed_region_is_refused_no_server() {
    let addr = start(config()).await;
    let mut eu = register(addr, "eu-1", "eu", "4").await;
    let _us = register(addr, "us-1", "us", "4").await;
    let (_host, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut eu).await;
    // One busy region and one free one.
    assert_eq!(regions(addr).await, pair("eu,us", "us"));

    let (mut late, reply) = create(addr, "eu").await;
    assert_eq!(reply["type"], "refused", "{reply:?}");
    assert_eq!(reply["reason"], "no_server");
    assert_eq!(expect_close(&mut late).await, Some(1000));

    let (_other, joined) = create(addr, "us").await;
    assert_eq!(joined["type"], "joined");
    assert_eq!(regions(addr).await, pair("eu,us", ""));
}

// --- create, join, relay --------------------------------------------------

#[tokio::test]
async fn create_join_and_relay_signaling_both_ways() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;

    let (mut a, joined) = create(addr, "eu").await;
    let (code, peer_a, token_a) = expect_room(&mut server).await;
    assert_eq!(joined["type"], "joined");
    assert_eq!(joined["code"], code);
    assert_eq!(joined["token"], token_a);
    assert_eq!(joined["region"], "eu");
    assert_eq!(code.len(), 5);
    assert!(code
        .bytes()
        .all(|b| b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789".contains(&b)));
    assert_eq!(token_a.len(), 32);
    assert!(token_a
        .bytes()
        .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)));

    // Client → server: the exact original text inside a signal envelope.
    let offer = r#"{"type":"offer","sdp":"v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\n\"quoted\" é"}"#;
    send(&mut a, offer).await;
    let signal = recv(&mut server).await;
    assert_eq!(signal["type"], "signal");
    assert_eq!(signal["peer"], peer_a);
    assert_eq!(signal["msg"], offer);

    // Server → client: unwrapped, unchanged.
    let answer = r#"{"type":"answer","sdp":"v=0\r\na=x\r\n"}"#;
    send(
        &mut server,
        json(&[("type", "signal"), ("peer", &peer_a), ("msg", answer)]),
    )
    .await;
    assert_eq!(recv_text(&mut a).await, answer);

    // A second client joins with a lower-case code.
    let (mut b, joined_b) = join(addr, &code.to_lowercase()).await;
    let (peer_b, token_b) = expect_peer(&mut server).await;
    assert_eq!(joined_b["type"], "joined");
    assert_eq!(joined_b["code"], code);
    assert_eq!(joined_b["token"], token_b);
    assert_ne!(peer_a, peer_b);
    assert_ne!(token_a, token_b);

    let candidate =
        r#"{"type":"candidate","candidate":"candidate:1 1 UDP 1 10.0.0.1 9 typ host","mid":"0"}"#;
    send(&mut b, candidate).await;
    let signal = recv(&mut server).await;
    assert_eq!(signal["peer"], peer_b);
    assert_eq!(signal["msg"], candidate);

    // Server candidates go to the addressed peer only.
    send(
        &mut server,
        json(&[("type", "signal"), ("peer", &peer_b), ("msg", candidate)]),
    )
    .await;
    assert_eq!(recv_text(&mut b).await, candidate);
    expect_silence(&mut a, Duration::from_millis(200)).await;

    let health = healthz(addr).await;
    assert_eq!(health["servers"], 1);
    assert_eq!(health["rooms"], 1);
    assert_eq!(health["regions"], serde_json::json!([]));
}

#[tokio::test]
async fn create_picks_the_lowest_name_among_free_servers() {
    let addr = start(config()).await;
    let mut eu2 = register(addr, "eu-2", "eu", "4").await;
    let mut eu1 = register(addr, "eu-1", "eu", "4").await;
    let mut busy = register(addr, "eu-0", "eu", "0").await;

    let (_a, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut eu1).await;

    let (_b, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut eu2).await;
    expect_silence(&mut busy, Duration::from_millis(100)).await;
}

// --- refusals -------------------------------------------------------------

#[tokio::test]
async fn refusal_reasons() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "1").await;

    // unknown_code
    let (mut ws, reply) = join(addr, "ZZZZZ").await;
    assert_eq!(reply["reason"], "unknown_code");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    // no_server: an unknown region.
    let (mut ws, reply) = create(addr, "mars").await;
    assert_eq!(reply["reason"], "no_server");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    // mismatch on create: the only free server has another build.
    let (mut ws, reply) = request(
        addr,
        &[("type", "create"), ("region", "eu"), ("build", "ffffffff")],
    )
    .await;
    assert_eq!(reply["reason"], "mismatch");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    let (_owner, joined) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;
    assert_eq!(joined["type"], "joined");

    // no_server: the region's only server is busy.
    let (mut ws, reply) = create(addr, "eu").await;
    assert_eq!(reply["reason"], "no_server");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    // full: capacity 1 and one client in the room.
    let (mut ws, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "full");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    // mismatch on join (after making room for one more).
    send(
        &mut server,
        json(&[("type", "capacity"), ("capacity", "4")]),
    )
    .await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (mut ws, reply) = request(
        addr,
        &[
            ("type", "join"),
            ("code", &code),
            ("build", BUILD),
            ("content", "deadbeef"),
        ],
    )
    .await;
    assert_eq!(reply["reason"], "mismatch");
    assert_eq!(expect_close(&mut ws).await, Some(1000));

    // bad_request: unknown type, missing keys.
    for pairs in [
        &[("type", "hello")][..],
        &[("type", "create")][..],
        &[("type", "join")][..],
        &[("region", "eu")][..],
        &[("type", "offer"), ("sdp", "v=0")][..],
    ] {
        let (mut ws, reply) = request(addr, pairs).await;
        assert_eq!(reply["type"], "refused", "{pairs:?}");
        assert_eq!(reply["reason"], "bad_request", "{pairs:?}");
        assert_eq!(expect_close(&mut ws).await, Some(1000));
    }

    // bad_request: a second request on a joined socket; the server hears
    // peer_gone.
    let (mut second, joined) = join(addr, &code).await;
    assert_eq!(joined["type"], "joined");
    let (peer, _) = expect_peer(&mut server).await;
    send(&mut second, json(&[("type", "join"), ("code", &code)])).await;
    expect_refused(&mut second, "bad_request").await;
    let gone = recv(&mut server).await;
    assert_eq!(gone["type"], "peer_gone");
    assert_eq!(gone["peer"], peer);
}

#[tokio::test]
async fn frames_that_are_not_flat_objects_of_strings_close_with_1002() {
    let addr = start(config()).await;
    for frame in [
        r#"{"type":"create","region":{"name":"eu"}}"#,
        r#"{"type":"create","region":7}"#,
        r#"{"type":"regions","x":null}"#,
        r#"["regions"]"#,
        "not json",
    ] {
        let mut ws = connect(addr, "/client").await;
        send(&mut ws, frame).await;
        assert_eq!(expect_close(&mut ws).await, Some(1002), "{frame}");
    }

    let mut ws = connect(addr, "/client").await;
    ws.send(Message::binary(b"{}".to_vec())).await.unwrap();
    assert_eq!(expect_close(&mut ws).await, Some(1002));

    let mut ws = connect(addr, "/server").await;
    send(&mut ws, r#"{"type":"register","capacity":4}"#).await;
    assert_eq!(expect_close(&mut ws).await, Some(1002));

    // After registration too, and the server's unknown messages.
    let mut server = register(addr, "eu-1", "eu", "4").await;
    send(&mut server, r#"{"type":"capacity","capacity":5}"#).await;
    assert_eq!(expect_close(&mut server).await, Some(1002));
    let mut server = register(addr, "eu-1", "eu", "4").await;
    send(&mut server, json(&[("type", "register")])).await;
    assert_eq!(expect_close(&mut server).await, Some(1002));

    // A joined client's frame that is not flat.
    let mut server = register(addr, "eu-2", "eu", "4").await;
    let (mut client, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    send(&mut client, r#"{"type":"offer","sdp":["v=0"]}"#).await;
    assert_eq!(expect_close(&mut client).await, Some(1002));
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
}

// --- lifecycle ------------------------------------------------------------

#[tokio::test]
async fn client_close_sends_peer_gone() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (_, peer, _) = expect_room(&mut server).await;
    a.close(None).await.unwrap();
    let gone = recv(&mut server).await;
    assert_eq!(gone["type"], "peer_gone");
    assert_eq!(gone["peer"], peer);
    // The room lives on (no expiry yet).
    assert_eq!(healthz(addr).await["rooms"], 1);
}

#[tokio::test]
async fn server_close_sends_closed_server_gone() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;
    let (mut b, _) = join(addr, &code).await;
    expect_peer(&mut server).await;

    drop(server);
    for client in [&mut a, &mut b] {
        let closed = recv(client).await;
        assert_eq!(closed["type"], "closed");
        assert_eq!(closed["reason"], "server_gone");
        assert_eq!(expect_close(client).await, Some(1000));
    }
    let health = healthz(addr).await;
    assert_eq!(health["servers"], 0);
    assert_eq!(health["rooms"], 0);
    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "unknown_code");
}

#[tokio::test]
async fn server_peer_gone_kicks_the_client() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (_, peer, _) = expect_room(&mut server).await;
    send(&mut server, json(&[("type", "peer_gone"), ("peer", &peer)])).await;
    let closed = recv(&mut a).await;
    assert_eq!(closed["type"], "closed");
    assert_eq!(closed["reason"], "kicked");
    assert_eq!(expect_close(&mut a).await, Some(1000));
    // The server dropped it itself: no peer_gone echoed back.
    expect_silence(&mut server, Duration::from_millis(200)).await;
}

#[tokio::test]
async fn empty_room_expires_and_frees_the_server() {
    let ttl = Duration::from_millis(300);
    let addr = start(config().with_room_ttl(ttl)).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;

    let (mut a, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;

    // A second create cannot reuse the server while its room lives.
    let (_, reply) = create(addr, "eu").await;
    assert_eq!(reply["reason"], "no_server");

    a.close(None).await.unwrap();
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
    let closed = recv(&mut server).await;
    assert_eq!(closed["type"], "room_closed");
    assert_eq!(closed["code"], code);
    assert_eq!(healthz(addr).await["rooms"], 0);

    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "unknown_code");

    // Now the server is free for a new room.
    let (_b, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    let (new_code, _, _) = expect_room(&mut server).await;
    assert_eq!(joined["code"], new_code);
}

#[tokio::test]
async fn a_join_resets_the_expiry_timer() {
    let ttl = Duration::from_millis(400);
    let addr = start(config().with_room_ttl(ttl)).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;
    a.close(None).await.unwrap();
    assert_eq!(recv(&mut server).await["type"], "peer_gone");

    tokio::time::sleep(ttl / 4).await;
    let (_b, joined) = join(addr, &code).await;
    assert_eq!(joined["type"], "joined");
    expect_peer(&mut server).await;
    // Well past the first timer: still no room_closed.
    expect_silence(&mut server, ttl * 2).await;
    assert_eq!(healthz(addr).await["rooms"], 1);
}

// --- rejoin (v1.2) ---------------------------------------------------------

async fn rejoin(addr: SocketAddr, code: &str, token: &str) -> (Ws, Fields) {
    request(
        addr,
        &[
            ("type", "join"),
            ("code", code),
            ("token", token),
            ("build", BUILD),
            ("content", CONTENT),
        ],
    )
    .await
}

#[tokio::test]
async fn a_rejoin_after_leaving_gets_the_same_token() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "2").await;
    let (mut a, created) = create(addr, "eu").await;
    let (code, peer, token) = expect_room(&mut server).await;
    assert_eq!(created["token"], token);
    let (mut b, joined) = join(addr, &code).await;
    let (b_peer, b_token) = expect_peer(&mut server).await;
    assert_eq!(joined["token"], b_token);

    // a's socket closes (its network dropped): the room keeps its token.
    a.close(None).await.unwrap();
    let gone = recv(&mut server).await;
    assert_eq!(
        (gone["type"].as_str(), gone["peer"].as_str()),
        ("peer_gone", peer.as_str())
    );

    // A third player fills the room to capacity: a plain join is full, but
    // a's rejoin is not counted against the capacity.
    let (_c, reply) = join(addr, &code).await;
    assert_eq!(reply["type"], "joined");
    expect_peer(&mut server).await;
    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "full");

    let (_a2, again) = rejoin(addr, &code, &token).await;
    assert_eq!(again["type"], "joined", "{again:?}");
    assert_eq!(again["token"], token);
    assert_eq!(again["code"], code);
    let (new_peer, same) = expect_peer(&mut server).await;
    assert_eq!(same, token);
    assert_ne!(new_peer, peer);

    // A kicked player may rejoin too (the server dropped it on a timeout).
    send(
        &mut server,
        json(&[("type", "peer_gone"), ("peer", &b_peer)]),
    )
    .await;
    assert_eq!(recv(&mut b).await["reason"], "kicked");
    let (_b2, again) = rejoin(addr, &code, &b_token).await;
    assert_eq!(again["token"], b_token);
    assert_eq!(expect_peer(&mut server).await.1, b_token);
}

#[tokio::test]
async fn a_rejoin_replaces_a_socket_still_seated_under_the_token() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (code, peer, token) = expect_room(&mut server).await;

    let (_a2, again) = rejoin(addr, &code, &token).await;
    assert_eq!(again["type"], "joined");
    // The old socket is told and closed; the server hears it left first.
    let closed = recv(&mut a).await;
    assert_eq!(closed["type"], "closed");
    assert_eq!(closed["reason"], "replaced");
    assert_eq!(expect_close(&mut a).await, Some(1000));
    let gone = recv(&mut server).await;
    assert_eq!(
        (gone["type"].as_str(), gone["peer"].as_str()),
        ("peer_gone", peer.as_str())
    );
    let (new_peer, same) = expect_peer(&mut server).await;
    assert_eq!(same, token);
    assert_ne!(new_peer, peer);
    // The replaced socket's close sends no second peer_gone.
    expect_silence(&mut server, Duration::from_millis(200)).await;
    assert_eq!(healthz(addr).await["rooms"], 1);
}

#[tokio::test]
async fn a_rejoin_with_a_foreign_or_malformed_token_is_refused() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (_a, _) = create(addr, "eu").await;
    let (code, _, token) = expect_room(&mut server).await;
    let other = "0123456789abcdef0123456789abcdef";
    assert_ne!(other, token);
    let (mut ws, reply) = rejoin(addr, &code, other).await;
    assert_eq!(reply["reason"], "bad_token");
    assert_eq!(expect_close(&mut ws).await, Some(1000));
    let (_, reply) = rejoin(addr, &code, "NOT-HEX").await;
    assert_eq!(reply["reason"], "bad_token");
    // A token of another room does not open this one.
    let mut server2 = register(addr, "eu-2", "eu", "4").await;
    let (_b, _) = create(addr, "eu").await;
    let (code2, _, _) = expect_room(&mut server2).await;
    let (_, reply) = rejoin(addr, &code2, &token).await;
    assert_eq!(reply["reason"], "bad_token");
    expect_silence(&mut server, Duration::from_millis(100)).await;
}

#[tokio::test]
async fn capacity_update_changes_full() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "2").await;
    let (_a, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;

    send(
        &mut server,
        json(&[("type", "capacity"), ("capacity", "1")]),
    )
    .await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "full");

    send(
        &mut server,
        json(&[("type", "capacity"), ("capacity", "3")]),
    )
    .await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_b, reply) = join(addr, &code).await;
    assert_eq!(reply["type"], "joined");
    let (_c, reply) = join(addr, &code).await;
    assert_eq!(reply["type"], "joined");
    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "full");
}

// --- limits ---------------------------------------------------------------

#[tokio::test]
async fn rate_limit_closes_the_socket() {
    let addr = start(config()).await;

    let mut server = register(addr, "eu-1", "eu", "4").await;
    for _ in 0..200 {
        if server
            .send(Message::text(json(&[
                ("type", "capacity"),
                ("capacity", "4"),
            ])))
            .await
            .is_err()
        {
            break;
        }
    }
    assert_eq!(expect_close(&mut server).await, Some(1002));

    let mut server = register(addr, "eu-2", "eu", "4").await;
    let (mut client, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    let candidate = json(&[("type", "candidate"), ("candidate", "c"), ("mid", "0")]);
    for _ in 0..200 {
        if client.send(Message::text(candidate.clone())).await.is_err() {
            break;
        }
    }
    assert_eq!(expect_close(&mut client).await, Some(1002));
    // The burst (minus the create) got through, then the client was dropped.
    let mut signals = 0;
    loop {
        let msg = recv(&mut server).await;
        if msg["type"] == "peer_gone" {
            break;
        }
        assert_eq!(msg["type"], "signal");
        signals += 1;
    }
    assert!((119..=125).contains(&signals), "{signals} signals relayed");
}

#[tokio::test]
async fn sustained_traffic_under_the_rate_is_fine() {
    let addr = start(config().with_rate_limit(60.0, 5.0)).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    for _ in 0..20 {
        send(
            &mut server,
            json(&[("type", "capacity"), ("capacity", "4")]),
        )
        .await;
        tokio::time::sleep(Duration::from_millis(25)).await;
    }
    expect_silence(&mut server, Duration::from_millis(100)).await;
    assert_eq!(healthz(addr).await["servers"], 1);
}

#[tokio::test]
async fn frame_size_limit() {
    let addr = start(config()).await;

    // Exactly 64 KiB is accepted (unknown keys are ignored).
    let base = json(&[("type", "regions"), ("pad", "")]);
    let pad = "x".repeat(64 * 1024 - base.len());
    let frame = json(&[("type", "regions"), ("pad", &pad)]);
    assert_eq!(frame.len(), 64 * 1024);
    let mut ws = connect(addr, "/client").await;
    send(&mut ws, frame).await;
    assert_eq!(recv(&mut ws).await["type"], "regions");

    // One byte more closes the socket.
    let frame = json(&[("type", "regions"), ("pad", &format!("{pad}x"))]);
    let mut ws = connect(addr, "/client").await;
    send(&mut ws, frame).await;
    assert_eq!(expect_close(&mut ws).await, Some(1002));

    // A joined client's oversized signaling too.
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut client, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    let big = json(&[("type", "offer"), ("sdp", &"v".repeat(70 * 1024))]);
    send(&mut client, big).await;
    assert_eq!(expect_close(&mut client).await, Some(1002));
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
}

/// The `signal` envelope the lobby wraps a client's frame in, as the server
/// receives it (same encoder, so the same length).
fn envelope(peer: &str, frame: &str) -> String {
    json(&[("type", "signal"), ("peer", peer), ("msg", frame)])
}

#[tokio::test]
async fn a_frame_whose_envelope_outgrows_the_limit_costs_only_its_client() {
    let addr = start(config()).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut a, _) = create(addr, "eu").await;
    let (code, peer_a, _) = expect_room(&mut server).await;
    let (mut b, _) = join(addr, &code).await;
    let (peer_b, _) = expect_peer(&mut server).await;

    // Under the inbound limit, but escaping it into `msg` doubles every
    // quote and backslash: the envelope would be about twice the limit.
    let frame = json(&[("type", "offer"), ("sdp", &"\"".repeat(30 * 1024))]);
    assert!(frame.len() <= 64 * 1024);
    assert!(envelope(&peer_b, &frame).len() > 64 * 1024);
    send(&mut b, frame).await;
    assert_eq!(expect_close(&mut b).await, Some(1002));
    // The server hears only that b left; nothing oversized reaches it.
    let gone = recv(&mut server).await;
    assert_eq!(
        (gone["type"].as_str(), gone["peer"].as_str()),
        ("peer_gone", peer_b.as_str())
    );

    // The room and a's relay live on, and an envelope of exactly the
    // limit still goes through.
    let base = envelope(&peer_a, &json(&[("type", "offer"), ("sdp", "")])).len();
    let sdp = "v".repeat(64 * 1024 - base);
    let fits = json(&[("type", "offer"), ("sdp", &sdp)]);
    assert_eq!(envelope(&peer_a, &fits).len(), 64 * 1024);
    send(&mut a, fits.clone()).await;
    let text = recv_text(&mut server).await;
    assert_eq!(text.len(), 64 * 1024);
    let signal: Fields = serde_json::from_str(&text).unwrap();
    assert_eq!(signal["peer"], peer_a);
    assert_eq!(signal["msg"], fits);
    // One byte more is a's protocol error, not the server's.
    let over = json(&[("type", "offer"), ("sdp", &format!("{sdp}v"))]);
    send(&mut a, over).await;
    assert_eq!(expect_close(&mut a).await, Some(1002));
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
    assert_eq!(healthz(addr).await["servers"], 1);
    let (_c, joined) = join(addr, &code).await;
    assert_eq!(joined["type"], "joined");
    expect_peer(&mut server).await;
}

#[tokio::test]
async fn the_regions_reply_fits_a_frame_at_the_region_limit() {
    let addr = start(config()).await;
    let mut servers = Vec::new();
    for i in 0..64 {
        // 32-character labels, the longest valid ones.
        let region = format!("{i:0>32}");
        servers.push(register(addr, &format!("s-{i}"), &region, "4").await);
    }
    // A 65th distinct region is refused; another server in a known one is not.
    let mut extra = connect(addr, "/server").await;
    send(&mut extra, register_msg("s-64", &"x".repeat(32), "4")).await;
    expect_refused(&mut extra, "bad_request").await;
    servers.push(register(addr, "s-65", &format!("{:0>32}", 0), "4").await);

    let mut ws = connect(addr, "/client").await;
    send(&mut ws, json(&[("type", "regions")])).await;
    let text = recv_text(&mut ws).await;
    assert!(text.len() <= 64 * 1024, "{} bytes", text.len());
    let reply: Fields = serde_json::from_str(&text).unwrap();
    assert_eq!(reply["list"].split(',').count(), 64);
    assert_eq!(reply["free"].split(',').count(), 64);
    // The bound itself: the configured maximum always fits.
    assert!(kfm_lobby::protocol::max_regions_in_a_frame(64 * 1024) >= 64);
}

#[tokio::test]
async fn first_message_timeout() {
    let timeout = Duration::from_millis(200);
    let addr = start(config().with_first_message_timeout(timeout)).await;
    for path in ["/client", "/server"] {
        let mut ws = connect(addr, path).await;
        let started = tokio::time::Instant::now();
        assert_eq!(expect_close(&mut ws).await, Some(1002), "{path}");
        assert!(started.elapsed() >= timeout - Duration::from_millis(20));
    }

    // A joined client is not subject to it.
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (mut client, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    expect_silence(&mut client, timeout * 2).await;
    expect_silence(&mut server, Duration::from_millis(10)).await;
}

#[tokio::test]
async fn healthz_counts() {
    let addr = start(config()).await;
    assert_eq!(
        healthz(addr).await,
        serde_json::json!({"servers": 0, "rooms": 0, "regions": []})
    );
    let mut eu = register(addr, "eu-1", "eu", "4").await;
    let us = register(addr, "us-1", "us", "4").await;
    assert_eq!(
        healthz(addr).await,
        serde_json::json!({"servers": 2, "rooms": 0, "regions": ["eu", "us"]})
    );
    let (_a, _) = create(addr, "eu").await;
    expect_room(&mut eu).await;
    assert_eq!(
        healthz(addr).await,
        serde_json::json!({"servers": 2, "rooms": 1, "regions": ["us"]})
    );
    drop(us);
    wait_health(addr, |h| h["servers"] == 1).await;
    assert_eq!(
        healthz(addr).await,
        serde_json::json!({"servers": 1, "rooms": 1, "regions": []})
    );
    drop(eu);
    wait_health(addr, |h| h["servers"] == 0 && h["rooms"] == 0).await;
}

#[test]
fn close_codes_are_the_documented_ones() {
    assert_eq!(
        u16::from(CloseCode::Normal),
        kfm_lobby::protocol::CLOSE_NORMAL
    );
    assert_eq!(
        u16::from(CloseCode::Protocol),
        kfm_lobby::protocol::CLOSE_PROTOCOL_ERROR
    );
}

// --- abuse limits (v1.3, §7) -----------------------------------------------

/// A `/client` socket that the lobby, trusting 127.0.0.1 as its proxy, sees
/// as coming from `ip`.
async fn connect_as(addr: SocketAddr, ip: &str) -> Ws {
    use tokio_tungstenite::tungstenite::client::IntoClientRequest;
    let mut request = format!("ws://{addr}/client").into_client_request().unwrap();
    request.headers_mut().insert(
        "x-forwarded-for",
        format!("10.9.9.9, {ip}").parse().unwrap(),
    );
    let (ws, _) = tokio_tungstenite::connect_async(request).await.unwrap();
    ws
}

async fn request_as(addr: SocketAddr, ip: &str, pairs: &[(&str, &str)]) -> (Ws, Fields) {
    let mut ws = connect_as(addr, ip).await;
    send(&mut ws, json(pairs)).await;
    let reply = recv(&mut ws).await;
    (ws, reply)
}

fn create_msg(region: &str) -> [(&str, &str); 4] {
    [
        ("type", "create"),
        ("region", region),
        ("build", BUILD),
        ("content", CONTENT),
    ]
}

fn join_msg(code: &str) -> [(&str, &str); 4] {
    [
        ("type", "join"),
        ("code", code),
        ("build", BUILD),
        ("content", CONTENT),
    ]
}

fn proxied() -> LobbyConfig {
    config().with_trusted_proxies(vec!["127.0.0.1".parse().unwrap()])
}

/// A v1.3 server: its `register` carries `version`, so it reports `players`.
async fn register_v13(addr: SocketAddr, name: &str, capacity: &str) -> Ws {
    let mut ws = connect(addr, "/server").await;
    let mut fields: Fields = serde_json::from_str(&register_msg(name, "eu", capacity)).unwrap();
    fields.insert("version".into(), "1.3".into());
    send(&mut ws, serde_json::to_string(&fields).unwrap()).await;
    let reply = recv(&mut ws).await;
    assert_eq!(reply["type"], "registered", "{reply:?}");
    assert_eq!(reply["version"], "1.3");
    ws
}

#[tokio::test]
async fn registered_names_the_lobby_version() {
    let addr = start(config()).await;
    let mut ws = connect(addr, "/server").await;
    send(&mut ws, register_msg("eu-1", "eu", "4")).await;
    let reply = recv(&mut ws).await;
    assert_eq!(reply["type"], "registered");
    assert_eq!(reply["version"], "1.3");
}

#[tokio::test]
async fn open_sockets_are_capped_per_client_address() {
    let mut config = proxied();
    config.max_connections_per_address = 2;
    let addr = start(config).await;
    let a = connect_as(addr, "192.0.2.1").await;
    let _b = connect_as(addr, "192.0.2.1").await;
    // The third from that address is refused at once, without a request.
    let mut c = connect_as(addr, "192.0.2.1").await;
    expect_refused(&mut c, "limited").await;
    // Another address is not affected, and a closed socket frees a place.
    let (_, reply) = request_as(addr, "192.0.2.2", &[("type", "regions")]).await;
    assert_eq!(reply["type"], "regions");
    drop(a);
    tokio::time::sleep(Duration::from_millis(100)).await;
    let (_, reply) = request_as(addr, "192.0.2.1", &[("type", "regions")]).await;
    assert_eq!(reply["type"], "regions");
}

#[tokio::test]
async fn forwarded_for_counts_only_from_a_trusted_proxy() {
    let mut config = config(); // no trusted proxy
    config.max_connections_per_address = 2;
    let addr = start(config).await;
    let _a = connect_as(addr, "192.0.2.1").await;
    let _b = connect_as(addr, "192.0.2.2").await;
    // Both counted as 127.0.0.1, whatever their header says.
    let mut c = connect_as(addr, "192.0.2.3").await;
    expect_refused(&mut c, "limited").await;
}

#[tokio::test]
async fn rooms_held_are_capped_per_client_address() {
    let mut config = proxied().with_room_ttl(Duration::from_millis(200));
    config.max_rooms_per_address = 1;
    let addr = start(config).await;
    let mut s1 = register(addr, "eu-1", "eu", "4").await;
    let mut s2 = register(addr, "eu-2", "eu", "4").await;
    let (first, joined) = request_as(addr, "192.0.2.1", &create_msg("eu")).await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut s1).await;
    // A free server is left, but this address already holds a room.
    let (_, reply) = request_as(addr, "192.0.2.1", &create_msg("eu")).await;
    assert_eq!(reply["reason"], "limited");
    let (_other, joined) = request_as(addr, "192.0.2.2", &create_msg("eu")).await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut s2).await;
    // Once its room closes the address may create again.
    drop(first);
    assert_eq!(recv(&mut s1).await["type"], "peer_gone");
    assert_eq!(recv(&mut s1).await["type"], "room_closed");
    let (_again, joined) = request_as(addr, "192.0.2.1", &create_msg("eu")).await;
    assert_eq!(joined["type"], "joined");
}

#[tokio::test]
async fn create_and_join_requests_are_rate_limited_per_address() {
    let mut config = proxied();
    config.requests_per_minute = 1.0;
    config.requests_burst = 3.0;
    let addr = start(config).await;
    for _ in 0..3 {
        let (_, reply) = request_as(addr, "192.0.2.1", &create_msg("mars")).await;
        assert_eq!(reply["reason"], "no_server");
    }
    let (_, reply) = request_as(addr, "192.0.2.1", &join_msg("ZZZZZ")).await;
    assert_eq!(reply["reason"], "limited");
    // A regions query is no request, and other addresses have their own.
    let (_, reply) = request_as(addr, "192.0.2.1", &[("type", "regions")]).await;
    assert_eq!(reply["type"], "regions");
    let (_, reply) = request_as(addr, "192.0.2.2", &create_msg("mars")).await;
    assert_eq!(reply["reason"], "no_server");
}

#[tokio::test]
async fn failed_joins_run_out_per_address() {
    let mut config = proxied();
    config.failed_joins_burst = 2.0;
    config.failed_joins_per_minute = 0.0;
    let addr = start(config).await;
    let mut server = register(addr, "eu-1", "eu", "4").await;
    let (_host, _) = request_as(addr, "192.0.2.9", &create_msg("eu")).await;
    let (code, _, _) = expect_room(&mut server).await;
    let (_, reply) = request_as(addr, "192.0.2.1", &join_msg("ZZZZZ")).await;
    assert_eq!(reply["reason"], "unknown_code");
    let bad_token = [
        ("type", "join"),
        ("code", code.as_str()),
        ("token", "0123456789abcdef0123456789abcdef"),
    ];
    let (_, reply) = request_as(addr, "192.0.2.1", &bad_token).await;
    assert_eq!(reply["reason"], "bad_token");
    // Out of guesses: even the right code is refused, before any lookup.
    let (_, reply) = request_as(addr, "192.0.2.1", &join_msg(&code)).await;
    assert_eq!(reply["reason"], "limited");
    expect_silence(&mut server, Duration::from_millis(100)).await;
    // A successful join spends no guess.
    let (_ok, reply) = request_as(addr, "192.0.2.2", &join_msg(&code)).await;
    assert_eq!(reply["type"], "joined");
    let (_ok2, reply) = request_as(addr, "192.0.2.2", &join_msg(&code)).await;
    assert_eq!(reply["type"], "joined");
}

#[tokio::test]
async fn a_room_keeps_a_bounded_set_of_tokens() {
    let mut config = config();
    config.max_clients_per_room = 2;
    config.max_tokens_per_room = 2;
    let addr = start(config).await;
    let mut server = register(addr, "eu-1", "eu", "8").await;
    let (_a, created) = create(addr, "eu").await;
    let (code, _, token_a) = expect_room(&mut server).await;
    assert_eq!(created["token"], token_a);
    let (b, _) = join(addr, &code).await;
    let (_, token_b) = expect_peer(&mut server).await;
    drop(b);
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
    // A third player: the room is at its two tokens, so b's (unseated, the
    // oldest such) makes way; a's, still seated, stays.
    let (_c, joined) = join(addr, &code).await;
    assert_eq!(joined["type"], "joined");
    expect_peer(&mut server).await;
    let (_, reply) = rejoin(addr, &code, &token_b).await;
    assert_eq!(reply["reason"], "bad_token");
    let (_a2, again) = rejoin(addr, &code, &token_a).await;
    assert_eq!(again["type"], "joined", "{again:?}");
}

#[tokio::test]
async fn a_room_no_game_connection_reaches_is_released_early() {
    let unused = Duration::from_millis(300);
    let addr = start(config().with_unused_room_ttl(unused)).await;
    let mut server = register_v13(addr, "eu-1", "4").await;

    // The creator keeps its socket open but never connects: released.
    let (mut idle, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;
    let closed = recv(&mut idle).await;
    assert_eq!(
        (closed["type"].as_str(), closed["reason"].as_str()),
        ("closed", "room_closed")
    );
    assert_eq!(expect_close(&mut idle).await, Some(1000));
    let msg = recv(&mut server).await;
    // (No peer_gone for the closed socket: the room went first.)
    assert_eq!(
        (msg["type"].as_str(), msg["code"].as_str()),
        ("room_closed", code.as_str())
    );
    assert_eq!(healthz(addr).await["rooms"], 0);

    // A room whose server reports a player is held past the short hold, and
    // then on the full TTL after everyone has gone.
    let (mut player, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    send(&mut server, json(&[("type", "players"), ("count", "1")])).await;
    expect_silence(&mut server, unused * 2).await;
    player.close(None).await.unwrap();
    assert_eq!(recv(&mut server).await["type"], "peer_gone");
    // Still a player on its game connection: no expiry at all.
    expect_silence(&mut server, unused * 2).await;
    send(&mut server, json(&[("type", "players"), ("count", "0")])).await;
    // The full TTL (600 s here): nothing within the short hold.
    expect_silence(&mut server, unused * 2).await;
    assert_eq!(healthz(addr).await["rooms"], 1);
}

#[tokio::test]
async fn an_older_server_keeps_the_full_hold() {
    let unused = Duration::from_millis(200);
    let addr = start(config().with_unused_room_ttl(unused)).await;
    let mut server = register(addr, "eu-1", "eu", "4").await; // no `version`
    let (mut a, _) = create(addr, "eu").await;
    expect_room(&mut server).await;
    expect_silence(&mut a, unused * 2).await;
    expect_silence(&mut server, Duration::from_millis(10)).await;
    assert_eq!(healthz(addr).await["rooms"], 1);
}

// --- room claims (v1.3, §5) -------------------------------------------------

const TOKEN_1: &str = "11111111111111111111111111111111";
const TOKEN_2: &str = "22222222222222222222222222222222";

/// A v1.3 `register` claiming `code` with these tokens and players.
fn claim_msg(name: &str, capacity: &str, code: &str, tokens: &str, players: &str) -> String {
    let mut fields: Fields = serde_json::from_str(&register_msg(name, "eu", capacity)).unwrap();
    for (k, v) in [
        ("version", "1.3"),
        ("room", code),
        ("tokens", tokens),
        ("players", players),
    ] {
        fields.insert(k.into(), v.into());
    }
    serde_json::to_string(&fields).unwrap()
}

async fn register_claiming(
    addr: SocketAddr,
    name: &str,
    code: &str,
    tokens: &str,
    players: &str,
) -> (Ws, Fields) {
    let mut ws = connect(addr, "/server").await;
    send(&mut ws, claim_msg(name, "4", code, tokens, players)).await;
    let reply = recv(&mut ws).await;
    assert_eq!(reply["type"], "registered", "{reply:?}");
    (ws, reply)
}

#[tokio::test]
async fn a_server_claims_its_room_back_after_a_lobby_restart() {
    // A fresh lobby, as after a restart: it knows nothing until the server
    // registers with the room it still hosts.
    let addr = start(config()).await;
    let (mut server, reply) =
        register_claiming(addr, "eu-1", "ABCDE", &format!("{TOKEN_1},{TOKEN_2}"), "2").await;
    assert_eq!(reply["room"], "ABCDE");
    assert_eq!(reply["version"], "1.3");
    // No `room` message for an adopted claim, and the server is busy.
    expect_silence(&mut server, Duration::from_millis(100)).await;
    assert_eq!(
        healthz(addr).await,
        serde_json::json!({"servers": 1, "rooms": 1, "regions": []})
    );

    // Its players rejoin under their tokens, and others join by the code.
    let (_a, again) = rejoin(addr, "abcde", TOKEN_1).await;
    assert_eq!(again["type"], "joined", "{again:?}");
    assert_eq!(again["token"], TOKEN_1);
    let (peer, token) = expect_peer(&mut server).await;
    assert_eq!(token, TOKEN_1);
    assert_eq!(peer, "1");
    let (_b, joined) = join(addr, "ABCDE").await;
    assert_eq!(joined["type"], "joined");
    expect_peer(&mut server).await;
    let (_, reply) = rejoin(addr, "ABCDE", "33333333333333333333333333333333").await;
    assert_eq!(reply["reason"], "bad_token");

    // Reported players count toward full: 4 connected (the 2 kept and the 2
    // who came back) with 2 sockets is the capacity of 4.
    send(&mut server, json(&[("type", "players"), ("count", "4")])).await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_, reply) = join(addr, "ABCDE").await;
    assert_eq!(reply["reason"], "full");
}

#[tokio::test]
async fn an_adopted_room_without_players_expires_like_any_empty_room() {
    let ttl = Duration::from_millis(300);
    let addr = start(config().with_room_ttl(ttl)).await;
    let (mut server, reply) = register_claiming(addr, "eu-1", "ABCDE", TOKEN_1, "0").await;
    assert_eq!(reply["room"], "ABCDE");
    let closed = recv(&mut server).await;
    assert_eq!(
        (closed["type"].as_str(), closed["code"].as_str()),
        ("room_closed", "ABCDE")
    );
    // Then free for a create.
    let (_c, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut server).await;
}

#[tokio::test]
async fn a_claim_on_a_live_code_is_refused_and_the_server_stays_busy() {
    let addr = start(config()).await;
    let mut first = register(addr, "eu-1", "eu", "4").await;
    let (_host, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut first).await;

    // Another server claims that code (it was away while the code moved on).
    let (mut second, reply) = register_claiming(addr, "eu-2", &code, TOKEN_1, "1").await;
    assert!(!reply.contains_key("room"), "{reply:?}");
    assert_eq!(reply["version"], "1.3");
    // Registered at capacity 0: no create lands on it under its players.
    assert_eq!(regions(addr).await, pair("eu", ""));
    let (_, reply) = create(addr, "eu").await;
    assert_eq!(reply["reason"], "no_server");
    // Its players gone, it says so with its capacity and is free again.
    send(
        &mut second,
        json(&[("type", "capacity"), ("capacity", "4")]),
    )
    .await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_c, joined) = create(addr, "eu").await;
    assert_eq!(joined["type"], "joined");
    expect_room(&mut second).await;
    // The first room was never touched.
    let (_d, joined) = join(addr, &code).await;
    assert_eq!(joined["type"], "joined");
    expect_peer(&mut first).await;
}

#[tokio::test]
async fn claims_are_checked_and_need_a_version() {
    let addr = start(config()).await;
    for (code, tokens, players) in [
        ("abcde", TOKEN_1, "1"), // codes are upper case
        ("ABCD", TOKEN_1, "1"),
        ("ABCDE", "not-a-token", "1"),
        ("ABCDE", TOKEN_1, "many"),
    ] {
        let mut ws = connect(addr, "/server").await;
        send(&mut ws, claim_msg("eu-1", "4", code, tokens, players)).await;
        expect_refused(&mut ws, "bad_request").await;
    }
    // Claim keys from a server that names no version are ignored.
    let mut fields: Fields = serde_json::from_str(&register_msg("eu-1", "eu", "4")).unwrap();
    fields.insert("room".into(), "ABCDE".into());
    fields.insert("tokens".into(), TOKEN_1.into());
    let mut ws = connect(addr, "/server").await;
    send(&mut ws, serde_json::to_string(&fields).unwrap()).await;
    let reply = recv(&mut ws).await;
    assert_eq!(reply["type"], "registered");
    assert!(!reply.contains_key("room"));
    assert_eq!(healthz(addr).await["rooms"], 0);
}

#[tokio::test]
async fn reported_players_count_toward_full() {
    let addr = start(config()).await;
    let mut server = register_v13(addr, "eu-1", "2").await;
    let (_a, _) = create(addr, "eu").await;
    let (code, _, _) = expect_room(&mut server).await;
    // One socket, but the server has two players on their game connection.
    send(&mut server, json(&[("type", "players"), ("count", "2")])).await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_, reply) = join(addr, &code).await;
    assert_eq!(reply["reason"], "full");
    send(&mut server, json(&[("type", "players"), ("count", "1")])).await;
    tokio::time::sleep(Duration::from_millis(50)).await;
    let (_b, reply) = join(addr, &code).await;
    assert_eq!(reply["type"], "joined");
    expect_peer(&mut server).await;
    // A malformed count is the server's protocol error.
    send(&mut server, json(&[("type", "players"), ("count", "lots")])).await;
    assert_eq!(expect_close(&mut server).await, Some(1002));
}
