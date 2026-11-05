//! The lobby's state: the server registry and the rooms, behind one mutex.
//! Every method takes the lock once, never across an await, and talks to
//! sockets only through their unbounded outgoing queues, so the order of
//! messages to one socket is the order the state changed in.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::sync::{Arc, Mutex, MutexGuard};

use tokio::sync::mpsc;
use tracing::{debug, info, warn};

use crate::protocol::{self, CLOSE_NORMAL};
use crate::LobbyConfig;

/// Something for a socket's task to do.
#[derive(Debug)]
pub(crate) enum Out {
    /// Send this text frame.
    Text(String),
    /// Send a close frame with this code and end the connection.
    Close(u16),
}

pub(crate) type Tx = mpsc::UnboundedSender<Out>;

/// Why a client request was refused (the `reason` of `refused`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Refusal {
    UnknownCode,
    Full,
    NoServer,
    Mismatch,
    BadRequest,
    ServerGone,
    BadSecret,
}

impl Refusal {
    pub(crate) fn as_str(self) -> &'static str {
        match self {
            Refusal::UnknownCode => "unknown_code",
            Refusal::Full => "full",
            Refusal::NoServer => "no_server",
            Refusal::Mismatch => "mismatch",
            Refusal::BadRequest => "bad_request",
            Refusal::ServerGone => "server_gone",
            Refusal::BadSecret => "bad_secret",
        }
    }
}

/// What a registering server announced.
pub(crate) struct Registration {
    pub region: String,
    pub name: String,
    pub build: String,
    pub content: String,
    pub capacity: u32,
}

/// A client's place in a room after `joined`: the routing key of its socket.
#[derive(Debug, Clone)]
pub(crate) struct Seat {
    pub code: String,
    pub server: u64,
    pub peer: u64,
    pub token: String,
    pub region: String,
}

struct Server {
    tx: Tx,
    reg: Registration,
    room: Option<String>,
    next_peer: u64,
}

struct Room {
    server: u64,
    clients: BTreeMap<u64, Tx>,
    /// Generation of the pending expiry timer; any change voids older timers.
    expiry: u64,
}

#[derive(Default)]
struct State {
    next_server: u64,
    next_generation: u64,
    servers: HashMap<u64, Server>,
    rooms: HashMap<String, Room>,
}

impl State {
    fn generation(&mut self) -> u64 {
        self.next_generation += 1;
        self.next_generation
    }

    fn free_regions(&self) -> Vec<String> {
        let set: BTreeSet<&str> = self
            .servers
            .values()
            .filter(|s| s.room.is_none() && s.reg.capacity >= 1)
            .map(|s| s.reg.region.as_str())
            .collect();
        set.into_iter().map(str::to_owned).collect()
    }
}

/// Counts for `/healthz`.
pub(crate) struct Health {
    pub servers: usize,
    pub rooms: usize,
    pub regions: Vec<String>,
}

pub(crate) struct Lobby {
    pub config: LobbyConfig,
    state: Mutex<State>,
}

/// `true` when the client did not send the key or sent the server's value.
fn matches(client: Option<&str>, server: &str) -> bool {
    client.is_none_or(|c| c == server)
}

impl Lobby {
    pub(crate) fn new(config: LobbyConfig) -> Arc<Self> {
        Arc::new(Lobby {
            config,
            state: Mutex::new(State::default()),
        })
    }

    fn lock(&self) -> MutexGuard<'_, State> {
        // A panic while holding the lock leaves plain maps behind; keep going.
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub(crate) fn health(&self) -> Health {
        let state = self.lock();
        Health {
            servers: state.servers.len(),
            rooms: state.rooms.len(),
            regions: state.free_regions(),
        }
    }

    pub(crate) fn regions(&self) -> Vec<String> {
        self.lock().free_regions()
    }

    /// Adds a server to the registry and returns its id.
    pub(crate) fn register(&self, tx: Tx, reg: Registration) -> Result<u64, Refusal> {
        let mut state = self.lock();
        if state.servers.len() >= self.config.max_servers {
            warn!(event = "refuse", socket = "server", reason = "no_server", name = %reg.name, "server limit reached");
            return Err(Refusal::NoServer);
        }
        state.next_server += 1;
        let id = state.next_server;
        info!(
            event = "register",
            server = id,
            name = %reg.name,
            region = %reg.region,
            build = %reg.build,
            content = %reg.content,
            capacity = reg.capacity,
            "server registered"
        );
        state.servers.insert(
            id,
            Server {
                tx,
                reg,
                room: None,
                next_peer: 0,
            },
        );
        Ok(id)
    }

    /// Removes a server; its room's clients get `closed`/`server_gone`.
    pub(crate) fn unregister(&self, id: u64) {
        let mut state = self.lock();
        let Some(server) = state.servers.remove(&id) else {
            return;
        };
        info!(event = "unregister", server = id, name = %server.reg.name, "server left");
        if let Some(code) = server.room {
            if let Some(room) = state.rooms.remove(&code) {
                for tx in room.clients.values() {
                    let _ = tx.send(Out::Text(protocol::closed("server_gone")));
                    let _ = tx.send(Out::Close(CLOSE_NORMAL));
                }
                info!(event = "room_close", code = %code, server = id, reason = "server_gone", clients = room.clients.len(), "room closed");
            }
        }
    }

    /// Updates a server's free human slots.
    pub(crate) fn set_capacity(&self, id: u64, capacity: u32) {
        let mut state = self.lock();
        if let Some(server) = state.servers.get_mut(&id) {
            debug!(server = id, capacity, "capacity");
            server.reg.capacity = capacity;
        }
    }

    /// `create`: reserves a room on a free server of the region and seats
    /// the creator as its first peer.
    pub(crate) fn create(
        self: &Arc<Self>,
        tx: Tx,
        region: &str,
        build: Option<&str>,
        content: Option<&str>,
    ) -> Result<Seat, Refusal> {
        let mut state = self.lock();
        if state.rooms.len() >= self.config.max_rooms {
            return Err(Refusal::NoServer);
        }
        let free: Vec<(u64, &Server)> = state
            .servers
            .iter()
            .filter(|(_, s)| s.reg.region == region && s.room.is_none() && s.reg.capacity >= 1)
            .map(|(id, s)| (*id, s))
            .collect();
        if free.is_empty() {
            return Err(Refusal::NoServer);
        }
        let Some(server_id) = free
            .iter()
            .filter(|(_, s)| matches(build, &s.reg.build) && matches(content, &s.reg.content))
            .min_by(|(a_id, a), (b_id, b)| a.reg.name.cmp(&b.reg.name).then(a_id.cmp(b_id)))
            .map(|(id, _)| *id)
        else {
            return Err(Refusal::Mismatch);
        };

        let code = loop {
            let code = protocol::random_code();
            if !state.rooms.contains_key(&code) {
                break code;
            }
        };
        let generation = state.generation();
        let token = protocol::random_token();
        let server = state.servers.get_mut(&server_id).expect("chosen above");
        server.next_peer += 1;
        let peer = server.next_peer;
        let room_msg = protocol::encode(&[("type", "room"), ("code", &code)]);
        let peer_msg = protocol::encode(&[
            ("type", "peer"),
            ("peer", &peer.to_string()),
            ("token", &token),
        ]);
        if server.tx.send(Out::Text(room_msg)).is_err()
            || server.tx.send(Out::Text(peer_msg)).is_err()
        {
            return Err(Refusal::ServerGone);
        }
        server.room = Some(code.clone());
        let region = server.reg.region.clone();
        info!(event = "room_open", code = %code, server = server_id, name = %server.reg.name, region = %region, "room opened");
        info!(event = "join", code = %code, server = server_id, peer, clients = 1, "client joined");
        let mut clients = BTreeMap::new();
        clients.insert(peer, tx);
        state.rooms.insert(
            code.clone(),
            Room {
                server: server_id,
                clients,
                expiry: generation,
            },
        );
        Ok(Seat {
            code,
            server: server_id,
            peer,
            token,
            region,
        })
    }

    /// `join`: seats a client in a live room.
    pub(crate) fn join(
        self: &Arc<Self>,
        tx: Tx,
        code: &str,
        build: Option<&str>,
        content: Option<&str>,
    ) -> Result<Seat, Refusal> {
        let code = code.to_ascii_uppercase();
        let mut state = self.lock();
        let generation = state.generation();
        let State { servers, rooms, .. } = &mut *state;
        let Some(room) = rooms.get_mut(&code) else {
            return Err(Refusal::UnknownCode);
        };
        let Some(server) = servers.get_mut(&room.server) else {
            return Err(Refusal::ServerGone);
        };
        let occupancy = room.clients.len();
        if occupancy >= self.config.max_clients_per_room
            || occupancy >= server.reg.capacity as usize
        {
            return Err(Refusal::Full);
        }
        if !matches(build, &server.reg.build) || !matches(content, &server.reg.content) {
            return Err(Refusal::Mismatch);
        }
        server.next_peer += 1;
        let peer = server.next_peer;
        let token = protocol::random_token();
        let peer_msg = protocol::encode(&[
            ("type", "peer"),
            ("peer", &peer.to_string()),
            ("token", &token),
        ]);
        if server.tx.send(Out::Text(peer_msg)).is_err() {
            return Err(Refusal::ServerGone);
        }
        room.clients.insert(peer, tx);
        room.expiry = generation; // voids a pending expiry timer
        info!(event = "join", code = %code, server = room.server, peer, clients = room.clients.len(), "client joined");
        Ok(Seat {
            code,
            server: room.server,
            peer,
            token,
            region: server.reg.region.clone(),
        })
    }

    /// Client → server: wraps a signaling frame in a `signal` envelope.
    pub(crate) fn forward_to_server(&self, seat: &Seat, text: &str) {
        let state = self.lock();
        let seated = state
            .rooms
            .get(&seat.code)
            .is_some_and(|r| r.server == seat.server && r.clients.contains_key(&seat.peer));
        if !seated {
            return;
        }
        if let Some(server) = state.servers.get(&seat.server) {
            let msg = protocol::encode(&[
                ("type", "signal"),
                ("peer", &seat.peer.to_string()),
                ("msg", text),
            ]);
            let _ = server.tx.send(Out::Text(msg));
        }
    }

    /// Server → client: unwraps a `signal` envelope.
    pub(crate) fn forward_to_client(&self, server_id: u64, peer: &str, msg: String) {
        let state = self.lock();
        let tx = state
            .servers
            .get(&server_id)
            .and_then(|s| s.room.as_ref())
            .and_then(|code| state.rooms.get(code))
            .zip(peer.parse::<u64>().ok())
            .and_then(|(room, peer)| room.clients.get(&peer));
        match tx {
            Some(tx) => {
                let _ = tx.send(Out::Text(msg));
            }
            None => debug!(
                server = server_id,
                peer, "signal for an unknown peer dropped"
            ),
        }
    }

    /// A client socket closed: tells the server and, when the room is
    /// empty, starts its expiry timer.
    pub(crate) fn leave(self: &Arc<Self>, seat: &Seat) {
        let mut state = self.lock();
        let generation = state.generation();
        let State { servers, rooms, .. } = &mut *state;
        let Some(room) = rooms.get_mut(&seat.code) else {
            return;
        };
        if room.server != seat.server || room.clients.remove(&seat.peer).is_none() {
            return;
        }
        info!(event = "peer_loss", code = %seat.code, server = seat.server, peer = seat.peer, by = "client", clients = room.clients.len(), "client left");
        if let Some(server) = servers.get(&seat.server) {
            let msg = protocol::encode(&[("type", "peer_gone"), ("peer", &seat.peer.to_string())]);
            let _ = server.tx.send(Out::Text(msg));
        }
        if room.clients.is_empty() {
            room.expiry = generation;
            self.schedule_expiry(seat.code.clone(), generation);
        }
    }

    /// Server → lobby `peer_gone`: the server dropped a peer, so its client
    /// gets `closed`/`kicked`.
    pub(crate) fn kick(self: &Arc<Self>, server_id: u64, peer: &str) {
        let mut state = self.lock();
        let generation = state.generation();
        let State { servers, rooms, .. } = &mut *state;
        let Some(code) = servers.get(&server_id).and_then(|s| s.room.clone()) else {
            return;
        };
        let Some(room) = rooms.get_mut(&code) else {
            return;
        };
        let Some(tx) = peer
            .parse::<u64>()
            .ok()
            .and_then(|p| room.clients.remove(&p))
        else {
            debug!(server = server_id, peer, "peer_gone for an unknown peer");
            return;
        };
        let _ = tx.send(Out::Text(protocol::closed("kicked")));
        let _ = tx.send(Out::Close(CLOSE_NORMAL));
        info!(event = "peer_loss", code = %code, server = server_id, peer, by = "server", clients = room.clients.len(), "client kicked");
        if room.clients.is_empty() {
            room.expiry = generation;
            self.schedule_expiry(code, generation);
        }
    }

    fn schedule_expiry(self: &Arc<Self>, code: String, generation: u64) {
        let lobby = Arc::clone(self);
        let ttl = self.config.room_ttl;
        tokio::spawn(async move {
            tokio::time::sleep(ttl).await;
            lobby.expire(&code, generation);
        });
    }

    fn expire(&self, code: &str, generation: u64) {
        let mut state = self.lock();
        let live = state
            .rooms
            .get(code)
            .is_some_and(|r| r.expiry == generation && r.clients.is_empty());
        if !live {
            return;
        }
        let room = state.rooms.remove(code).expect("checked above");
        if let Some(server) = state.servers.get_mut(&room.server) {
            server.room = None;
            let msg = protocol::encode(&[("type", "room_closed"), ("code", code)]);
            let _ = server.tx.send(Out::Text(msg));
        }
        info!(event = "room_close", code = %code, server = room.server, reason = "expired", "room closed");
    }
}
