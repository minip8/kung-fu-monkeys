//! One task per WebSocket: it reads frames (rate limit, size limit, the
//! flat-object rule), writes what the state queues for it, and runs the
//! client (§3) or server (§5) side of the protocol.

use std::sync::Arc;
use std::time::Duration;

use axum::extract::ws::{CloseFrame, Message, WebSocket};
use tokio::sync::mpsc;
use tokio::time::Instant;
use tracing::{debug, info, warn};

use crate::lobby::{Lobby, Out, Refusal, Registration, Seat, Tx};
use crate::protocol::{self, Fields, CLOSE_NORMAL, CLOSE_PROTOCOL_ERROR};
use crate::LobbyConfig;

/// Token bucket: `rate` messages per second, `burst` at most.
struct Bucket {
    tokens: f64,
    rate: f64,
    burst: f64,
    last: Instant,
}

impl Bucket {
    fn new(config: &LobbyConfig) -> Self {
        Bucket {
            tokens: config.rate_burst,
            rate: config.rate_per_second,
            burst: config.rate_burst,
            last: Instant::now(),
        }
    }

    fn take(&mut self) -> bool {
        let now = Instant::now();
        let elapsed = now.duration_since(self.last).as_secs_f64();
        self.last = now;
        self.tokens = (self.tokens + elapsed * self.rate).min(self.burst);
        if self.tokens >= 1.0 {
            self.tokens -= 1.0;
            true
        } else {
            false
        }
    }
}

enum Event {
    /// A text frame that passed the rate and size limits.
    Frame(String),
    /// Over the rate limit, over the size limit or a binary frame.
    Violation(&'static str),
    /// The peer closed the socket or the connection failed.
    Gone,
    /// The state queued something for this socket.
    Out(Out),
    /// The first-message deadline passed.
    Timeout,
}

/// How a socket's conversation ended. The caller updates the state first
/// (so the other side hears about it at once) and then runs the close
/// handshake, which may wait on the peer.
enum Ending {
    /// The socket is gone or a send failed: nothing more to send.
    Gone,
    /// The state asked for a close with this code (its message went first).
    Close(u16),
    /// Reply `refused` and close with 1000.
    Refuse(Refusal),
    /// Close with 1002 and no message.
    ProtocolError(&'static str),
}

struct Conn {
    socket: WebSocket,
    tx: Tx,
    rx: mpsc::UnboundedReceiver<Out>,
    bucket: Bucket,
    max_frame_bytes: usize,
}

impl Conn {
    fn new(socket: WebSocket, config: &LobbyConfig) -> Self {
        let (tx, rx) = mpsc::unbounded_channel();
        Conn {
            socket,
            tx,
            rx,
            bucket: Bucket::new(config),
            max_frame_bytes: config.max_frame_bytes,
        }
    }

    async fn next(&mut self, deadline: Option<Instant>) -> Event {
        loop {
            let sleep = async {
                match deadline {
                    Some(at) => tokio::time::sleep_until(at).await,
                    None => std::future::pending().await,
                }
            };
            tokio::select! {
                incoming = self.socket.recv() => {
                    let message = match incoming {
                        None | Some(Err(_)) | Some(Ok(Message::Close(_))) => return Event::Gone,
                        Some(Ok(message)) => message,
                    };
                    if !self.bucket.take() {
                        return Event::Violation("rate limit");
                    }
                    match message {
                        Message::Text(text) if text.len() > self.max_frame_bytes => {
                            return Event::Violation("frame too large");
                        }
                        Message::Text(text) => return Event::Frame(text.as_str().to_owned()),
                        Message::Binary(_) => return Event::Violation("binary frame"),
                        // Pings are answered by the WebSocket layer.
                        _ => continue,
                    }
                }
                out = self.rx.recv() => {
                    // `self.tx` keeps the channel open, so `None` cannot happen.
                    if let Some(out) = out {
                        return Event::Out(out);
                    }
                }
                () = sleep => return Event::Timeout,
            }
        }
    }

    async fn send(&mut self, text: String) -> bool {
        self.socket.send(Message::Text(text.into())).await.is_ok()
    }

    /// Sends a close frame and waits briefly for the peer's reply.
    async fn close(mut self, code: u16) {
        let frame = CloseFrame {
            code,
            reason: "".into(),
        };
        if self.socket.send(Message::Close(Some(frame))).await.is_err() {
            return;
        }
        let _ = tokio::time::timeout(Duration::from_secs(2), async {
            while let Some(Ok(message)) = self.socket.recv().await {
                if matches!(message, Message::Close(_)) {
                    break;
                }
            }
        })
        .await;
    }

    async fn finish(mut self, side: &str, ending: Ending) {
        match ending {
            Ending::Gone => {}
            Ending::Close(code) => self.close(code).await,
            Ending::Refuse(reason) => {
                info!(
                    event = "refuse",
                    socket = side,
                    reason = reason.as_str(),
                    "request refused"
                );
                if self.send(protocol::refused(reason.as_str())).await {
                    self.close(CLOSE_NORMAL).await;
                }
            }
            Ending::ProtocolError(what) => {
                warn!(
                    event = "protocol_error",
                    socket = side,
                    what,
                    "closing socket"
                );
                self.close(CLOSE_PROTOCOL_ERROR).await;
            }
        }
    }

    /// Waits for the first frame until the deadline.
    async fn first(&mut self, deadline: Instant) -> Result<String, Ending> {
        loop {
            match self.next(Some(deadline)).await {
                Event::Frame(text) => return Ok(text),
                Event::Violation(what) => return Err(Ending::ProtocolError(what)),
                Event::Timeout => return Err(Ending::ProtocolError("no first message in time")),
                Event::Gone => return Err(Ending::Gone),
                Event::Out(_) => continue, // nothing addresses a socket before it is known
            }
        }
    }
}

fn get<'a>(fields: &'a Fields, key: &str) -> Option<&'a str> {
    fields.get(key).map(String::as_str)
}

/// `/client`: one request, one reply, then signaling until the socket closes.
pub(crate) async fn client(lobby: Arc<Lobby>, socket: WebSocket) {
    let mut conn = Conn::new(socket, &lobby.config);
    let deadline = Instant::now() + lobby.config.first_message_timeout;
    let fields = match conn.first(deadline).await {
        Ok(text) => protocol::parse(&text),
        Err(ending) => return conn.finish("client", ending).await,
    };
    let Some(fields) = fields else {
        let ending = Ending::ProtocolError("not a flat object of strings");
        return conn.finish("client", ending).await;
    };

    let build = get(&fields, "build");
    let content = get(&fields, "content");
    let result = match (
        get(&fields, "type"),
        get(&fields, "region"),
        get(&fields, "code"),
    ) {
        (Some("regions"), _, _) => {
            let list = lobby.regions().join(",");
            if conn
                .send(protocol::encode(&[("type", "regions"), ("list", &list)]))
                .await
            {
                conn.close(CLOSE_NORMAL).await;
            }
            return;
        }
        (Some("create"), Some(region), _) => lobby.create(conn.tx.clone(), region, build, content),
        (Some("join"), _, Some(code)) => lobby.join(conn.tx.clone(), code, build, content),
        _ => Err(Refusal::BadRequest),
    };
    let seat = match result {
        Ok(seat) => seat,
        Err(reason) => return conn.finish("client", Ending::Refuse(reason)).await,
    };

    let joined = protocol::encode(&[
        ("type", "joined"),
        ("code", &seat.code),
        ("token", &seat.token),
        ("region", &seat.region),
    ]);
    let ending = if conn.send(joined).await {
        relay_client(&lobby, &mut conn, &seat).await
    } else {
        Ending::Gone
    };
    lobby.leave(&seat);
    conn.finish("client", ending).await;
}

/// After `joined`: client signaling to the server, the server's to the client.
async fn relay_client(lobby: &Arc<Lobby>, conn: &mut Conn, seat: &Seat) -> Ending {
    loop {
        match conn.next(None).await {
            Event::Frame(text) => {
                let Some(fields) = protocol::parse(&text) else {
                    return Ending::ProtocolError("not a flat object of strings");
                };
                match get(&fields, "type") {
                    Some("offer" | "candidate") => lobby.forward_to_server(seat, &text),
                    // A second request on the same socket.
                    Some("regions" | "create" | "join") => {
                        return Ending::Refuse(Refusal::BadRequest)
                    }
                    _ => return Ending::ProtocolError("unexpected message"),
                }
            }
            Event::Out(Out::Text(text)) => {
                if !conn.send(text).await {
                    return Ending::Gone;
                }
            }
            Event::Out(Out::Close(code)) => return Ending::Close(code),
            Event::Violation(what) => return Ending::ProtocolError(what),
            Event::Gone | Event::Timeout => return Ending::Gone,
        }
    }
}

fn registration(fields: &Fields) -> Option<Registration> {
    let region = get(fields, "region")?;
    if !protocol::valid_region(region) {
        return None;
    }
    Some(Registration {
        region: region.to_owned(),
        name: get(fields, "name")?.to_owned(),
        build: get(fields, "build")?.to_owned(),
        content: get(fields, "content")?.to_owned(),
        capacity: get(fields, "capacity")?.parse::<u32>().ok()?,
    })
}

/// Checks a `register` frame; `Err` is the refusal.
fn check_register(lobby: &Lobby, fields: Option<Fields>) -> Result<Registration, Ending> {
    let Some(fields) = fields else {
        return Err(Ending::ProtocolError("not a flat object of strings"));
    };
    if get(&fields, "type") != Some("register") {
        return Err(Ending::Refuse(Refusal::BadRequest));
    }
    match get(&fields, "secret") {
        None => return Err(Ending::Refuse(Refusal::BadRequest)),
        Some(secret) if !protocol::secrets_equal(secret, &lobby.config.secret) => {
            return Err(Ending::Refuse(Refusal::BadSecret));
        }
        Some(_) => {}
    }
    registration(&fields).ok_or(Ending::Refuse(Refusal::BadRequest))
}

/// `/server`: `register`, then the server half of rooms and signaling.
pub(crate) async fn server(lobby: Arc<Lobby>, socket: WebSocket) {
    let mut conn = Conn::new(socket, &lobby.config);
    let deadline = Instant::now() + lobby.config.first_message_timeout;
    let fields = match conn.first(deadline).await {
        Ok(text) => protocol::parse(&text),
        Err(ending) => return conn.finish("server", ending).await,
    };
    let reg = match check_register(&lobby, fields) {
        Ok(reg) => reg,
        Err(ending) => return conn.finish("server", ending).await,
    };
    let id = match lobby.register(conn.tx.clone(), reg) {
        Ok(id) => id,
        Err(reason) => return conn.finish("server", Ending::Refuse(reason)).await,
    };
    let ending = if conn.send(protocol::encode(&[("type", "registered")])).await {
        relay_server(&lobby, &mut conn, id).await
    } else {
        Ending::Gone
    };
    lobby.unregister(id);
    conn.finish("server", ending).await;
}

async fn relay_server(lobby: &Arc<Lobby>, conn: &mut Conn, id: u64) -> Ending {
    loop {
        match conn.next(None).await {
            Event::Frame(text) => {
                let Some(fields) = protocol::parse(&text) else {
                    return Ending::ProtocolError("not a flat object of strings");
                };
                match (get(&fields, "type"), get(&fields, "peer")) {
                    (Some("signal"), Some(peer)) => {
                        let Some(msg) = get(&fields, "msg") else {
                            return Ending::ProtocolError("signal without msg");
                        };
                        if protocol::parse(msg).is_none() {
                            debug!(
                                server = id,
                                peer, "signal msg is not a flat object; dropped"
                            );
                            continue;
                        }
                        lobby.forward_to_client(id, peer, msg.to_owned());
                    }
                    (Some("peer_gone"), Some(peer)) => lobby.kick(id, peer),
                    (Some("capacity"), _) => {
                        let capacity = get(&fields, "capacity").and_then(|c| c.parse::<u32>().ok());
                        let Some(capacity) = capacity else {
                            return Ending::ProtocolError("bad capacity");
                        };
                        lobby.set_capacity(id, capacity);
                    }
                    _ => return Ending::ProtocolError("unexpected message"),
                }
            }
            Event::Out(Out::Text(text)) => {
                if !conn.send(text).await {
                    return Ending::Gone;
                }
            }
            Event::Out(Out::Close(code)) => return Ending::Close(code),
            Event::Violation(what) => return Ending::ProtocolError(what),
            Event::Gone | Event::Timeout => return Ending::Gone,
        }
    }
}
