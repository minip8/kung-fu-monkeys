//! The lobby's state: the server registry, the rooms and the per-address
//! limits, behind one mutex. Every method takes the lock once, never across
//! an await, and talks to sockets only through their unbounded outgoing
//! queues, so the order of messages to one socket is the order the state
//! changed in.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::net::IpAddr;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use tokio::sync::mpsc;
use tracing::{debug, info, warn};

use crate::limits::{Address, Bucket};
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
    /// v1.2: a rejoin with a token the room never issued.
    BadToken,
    /// v1.3: the client's address is over one of the §7 limits.
    Limited,
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
            Refusal::BadToken => "bad_token",
            Refusal::Limited => "limited",
        }
    }
}

/// v1.3: a room a registering server still hosts (§5 "Room claims").
pub(crate) struct Claim {
    pub code: String,
    /// The room's tokens the server still honours, oldest first.
    pub tokens: Vec<String>,
    pub players: u32,
}

/// What a registering server announced.
pub(crate) struct Registration {
    pub region: String,
    pub name: String,
    pub build: String,
    pub content: String,
    pub capacity: u32,
    /// v1.3: the server reports `players` (its `register` said `version`).
    pub reports_players: bool,
    /// v1.3: the room it still hosts, if any (taken by `register`).
    pub claim: Option<Claim>,
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

/// A token the room issued: the peer seated under it now (`None`: its
/// socket is gone) and when it was issued, for eviction.
struct Token {
    seated: Option<u64>,
    issued: u64,
}

struct Room {
    /// Unique per room for the lobby's lifetime (a code can be reused).
    id: u64,
    server: u64,
    clients: BTreeMap<u64, Tx>,
    /// Every token issued for this room, so a `join` with one rejoins (v1.2),
    /// at most `max_tokens_per_room` of them (v1.3).
    tokens: HashMap<String, Token>,
    /// Generation of the pending expiry timer; any change voids older timers.
    expiry: u64,
    /// The address that created the room, for its per-address count.
    creator: Option<IpAddr>,
    /// Players the server reports connected over their game connection
    /// (v1.3 `players`; always 0 for an older server).
    players: u32,
    /// A game connection was ever made (a `players` above 0), or the server
    /// predates v1.3 and cannot say: the room has the full `room_ttl`.
    used: bool,
}

impl Room {
    /// Occupancy for `full`: open client sockets, or the server's count of
    /// connected players when that is higher.
    fn occupancy(&self) -> usize {
        self.clients.len().max(self.players as usize)
    }

    /// Nobody here, by socket or by game connection: the expiry may run.
    fn empty(&self) -> bool {
        self.clients.is_empty() && self.players == 0
    }
}

#[derive(Default)]
struct State {
    next_server: u64,
    next_generation: u64,
    servers: HashMap<u64, Server>,
    rooms: HashMap<String, Room>,
    addresses: HashMap<IpAddr, Address>,
}

impl State {
    fn generation(&mut self) -> u64 {
        self.next_generation += 1;
        self.next_generation
    }

    /// The distinct regions of the servers `keep` accepts, sorted.
    fn regions_where(&self, keep: impl Fn(&Server) -> bool) -> Vec<String> {
        let set: BTreeSet<&str> = self
            .servers
            .values()
            .filter(|s| keep(s))
            .map(|s| s.reg.region.as_str())
            .collect();
        set.into_iter().map(str::to_owned).collect()
    }

    /// The regions with a server that can take a `create` now: no room and
    /// at least one human slot.
    fn free_regions(&self) -> Vec<String> {
        self.regions_where(|s| s.room.is_none() && s.reg.capacity >= 1)
    }

    /// Every registered region, free or busy (lobby protocol v1.1).
    fn all_regions(&self) -> Vec<String> {
        self.regions_where(|_| true)
    }

    /// A room is gone: its creator's count goes down.
    fn forget_room(&mut self, room: &Room) {
        if let Some(addr) = room.creator {
            if let Some(a) = self.addresses.get_mut(&addr) {
                a.rooms = a.rooms.saturating_sub(1);
            }
        }
    }
}

/// The `regions` reply: every registered region and the free subset.
pub(crate) struct Regions {
    pub list: Vec<String>,
    pub free: Vec<String>,
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

/// Issues `token` to `peer` in a room that keeps at most `max` tokens: when
/// it is full, the oldest token no socket is seated under makes room (its
/// player can no longer rejoin). `false` when every token is seated.
fn issue_token(
    tokens: &mut HashMap<String, Token>,
    token: &str,
    peer: u64,
    issued: u64,
    max: usize,
) -> bool {
    if let Some(t) = tokens.get_mut(token) {
        t.seated = Some(peer);
        return true;
    }
    if tokens.len() >= max {
        let oldest = tokens
            .iter()
            .filter(|(_, t)| t.seated.is_none())
            .min_by_key(|(_, t)| t.issued)
            .map(|(k, _)| k.clone());
        match oldest {
            Some(k) => {
                tokens.remove(&k);
            }
            None => return false,
        }
    }
    tokens.insert(
        token.to_owned(),
        Token {
            seated: Some(peer),
            issued,
        },
    );
    true
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

    pub(crate) fn regions(&self) -> Regions {
        let state = self.lock();
        Regions {
            list: state.all_regions(),
            free: state.free_regions(),
        }
    }

    // --- per-address limits (§7, v1.3) -----------------------------------

    /// A `/client` socket from `addr` opened; `Err(Limited)` when the address
    /// has too many open, or the lobby tracks too many addresses.
    pub(crate) fn connect(&self, addr: IpAddr) -> Result<(), Refusal> {
        let config = &self.config;
        let mut state = self.lock();
        if !state.addresses.contains_key(&addr)
            && state.addresses.len() >= config.max_tracked_addresses
        {
            state.addresses.retain(|_, a| !a.idle());
            if state.addresses.len() >= config.max_tracked_addresses {
                warn!(event = "refuse", socket = "client", reason = "limited", %addr, "address table full");
                return Err(Refusal::Limited);
            }
        }
        let a = state.addresses.entry(addr).or_insert_with(|| Address {
            connections: 0,
            rooms: 0,
            requests: Bucket::new(config.requests_per_minute / 60.0, config.requests_burst),
            failures: Bucket::new(
                config.failed_joins_per_minute / 60.0,
                config.failed_joins_burst,
            ),
        });
        if a.connections >= config.max_connections_per_address {
            debug!(%addr, connections = a.connections, "connection limit");
            return Err(Refusal::Limited);
        }
        a.connections += 1;
        Ok(())
    }

    /// The socket `connect` admitted closed.
    pub(crate) fn disconnect(&self, addr: IpAddr) {
        let mut state = self.lock();
        if let Some(a) = state.addresses.get_mut(&addr) {
            a.connections = a.connections.saturating_sub(1);
        }
    }

    /// One `create` or `join` from `addr`: the request bucket, and for a
    /// join the failed-join bucket (a guesser out of guesses is refused
    /// before the code is even looked up).
    pub(crate) fn admit_request(&self, addr: IpAddr, join: bool) -> Result<(), Refusal> {
        let mut state = self.lock();
        let Some(a) = state.addresses.get_mut(&addr) else {
            return Ok(()); // not admitted through connect (cannot happen)
        };
        if join && !a.failures.has() {
            debug!(%addr, "failed-join limit");
            return Err(Refusal::Limited);
        }
        if !a.requests.take() {
            debug!(%addr, "request limit");
            return Err(Refusal::Limited);
        }
        Ok(())
    }

    /// A join from `addr` was refused for its code or token: one guess spent.
    pub(crate) fn failed_join(&self, addr: IpAddr) {
        let mut state = self.lock();
        if let Some(a) = state.addresses.get_mut(&addr) {
            let _ = a.failures.take();
        }
    }

    // --- servers ---------------------------------------------------------

    /// Adds a server to the registry and returns its id and, when it claimed
    /// a room the lobby adopted (v1.3), that room's code.
    pub(crate) fn register(
        self: &Arc<Self>,
        tx: Tx,
        mut reg: Registration,
    ) -> Result<(u64, Option<String>), Refusal> {
        let mut state = self.lock();
        if state.servers.len() >= self.config.max_servers {
            warn!(event = "refuse", socket = "server", reason = "bad_request", name = %reg.name, "server limit reached");
            return Err(Refusal::BadRequest);
        }
        let new_region = !state.servers.values().any(|s| s.reg.region == reg.region);
        if new_region && state.all_regions().len() >= self.config.max_regions {
            warn!(event = "refuse", socket = "server", reason = "bad_request", name = %reg.name, region = %reg.region, "region limit reached");
            return Err(Refusal::BadRequest);
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
            v13 = reg.reports_players,
            claim = reg.claim.as_ref().map_or("", |c| c.code.as_str()),
            "server registered"
        );
        let claim = reg.claim.take();
        let adopted = match claim {
            Some(claim)
                if !state.rooms.contains_key(&claim.code)
                    && state.rooms.len() < self.config.max_rooms =>
            {
                Some(claim)
            }
            Some(claim) => {
                // Not this server's room here: busy until it says otherwise.
                warn!(event = "claim_refused", server = id, code = %claim.code, "room claim not adopted");
                reg.capacity = 0;
                None
            }
            None => None,
        };
        let room = adopted.as_ref().map(|c| c.code.clone());
        state.servers.insert(
            id,
            Server {
                tx,
                reg,
                room: room.clone(),
                next_peer: 0,
            },
        );
        if let Some(claim) = adopted {
            // The claim lists them oldest first: issue them in that order,
            // before any token this lobby issues from now on.
            let tokens = claim
                .tokens
                .into_iter()
                .map(|t| {
                    let issued = state.generation();
                    (
                        t,
                        Token {
                            seated: None,
                            issued,
                        },
                    )
                })
                .collect();
            let generation = state.generation();
            let room = Room {
                id: generation,
                server: id,
                clients: BTreeMap::new(),
                tokens,
                expiry: generation,
                creator: None,
                players: claim.players,
                used: true,
            };
            info!(event = "room_adopt", code = %claim.code, server = id, players = claim.players, tokens = room.tokens.len(), "room adopted");
            let empty = room.empty();
            state.rooms.insert(claim.code.clone(), room);
            if empty {
                self.schedule_expiry(claim.code, generation, self.config.room_ttl);
            }
        }
        Ok((id, room))
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
                state.forget_room(&room);
            }
        }
    }

    /// Updates the total of human slots a server offers (§5 `capacity`:
    /// never net of joined humans; occupancy is counted here, by sockets).
    pub(crate) fn set_capacity(&self, id: u64, capacity: u32) {
        let mut state = self.lock();
        if let Some(server) = state.servers.get_mut(&id) {
            debug!(server = id, capacity, "capacity");
            server.reg.capacity = capacity;
        }
    }

    /// v1.3 `players`: how many of its room's players the server has
    /// connected over their game connection. A room is held while it has
    /// any (or a client socket), and the first one makes it used.
    pub(crate) fn set_players(self: &Arc<Self>, id: u64, players: u32) {
        let mut state = self.lock();
        let generation = state.generation();
        let State { servers, rooms, .. } = &mut *state;
        let Some(code) = servers.get(&id).and_then(|s| s.room.clone()) else {
            return;
        };
        let Some(room) = rooms.get_mut(&code) else {
            return;
        };
        debug!(server = id, code = %code, players, "players");
        let was_empty = room.empty();
        room.players = players;
        if players > 0 {
            room.used = true;
            room.expiry = generation; // voids a pending expiry timer
        } else if room.empty() && !was_empty {
            room.expiry = generation;
            self.schedule_expiry(code, generation, self.config.room_ttl);
        }
    }

    // --- clients ---------------------------------------------------------

    /// `create`: reserves a room on a free server of the region and seats
    /// the creator as its first peer.
    pub(crate) fn create(
        self: &Arc<Self>,
        tx: Tx,
        addr: Option<IpAddr>,
        region: &str,
        build: Option<&str>,
        content: Option<&str>,
    ) -> Result<Seat, Refusal> {
        let mut state = self.lock();
        if let Some(addr) = addr {
            let held = state.addresses.get(&addr).map_or(0, |a| a.rooms);
            if held >= self.config.max_rooms_per_address {
                debug!(%addr, rooms = held, "room limit");
                return Err(Refusal::Limited);
            }
        }
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
        let used = !server.reg.reports_players;
        info!(event = "room_open", code = %code, server = server_id, name = %server.reg.name, region = %region, "room opened");
        info!(event = "join", code = %code, server = server_id, peer, clients = 1, "client joined");
        let mut clients = BTreeMap::new();
        clients.insert(peer, tx);
        let mut tokens = HashMap::new();
        tokens.insert(
            token.clone(),
            Token {
                seated: Some(peer),
                issued: generation,
            },
        );
        state.rooms.insert(
            code.clone(),
            Room {
                id: generation,
                server: server_id,
                clients,
                tokens,
                expiry: generation,
                creator: addr,
                players: 0,
                used,
            },
        );
        if let Some(addr) = addr {
            if let Some(a) = state.addresses.get_mut(&addr) {
                a.rooms += 1;
            }
        }
        if !used {
            // v1.3: a room no game connection reaches in time is released.
            self.schedule_unused_close(code.clone(), generation);
        }
        Ok(Seat {
            code,
            server: server_id,
            peer,
            token,
            region,
        })
    }

    /// `join`: seats a client in a live room; with `rejoin`, under a token
    /// the room issued before (v1.2).
    pub(crate) fn join(
        self: &Arc<Self>,
        tx: Tx,
        code: &str,
        build: Option<&str>,
        content: Option<&str>,
        rejoin: Option<&str>,
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
        // v1.2: a rejoin is the same player, whose slots the server holds.
        let seated = match rejoin {
            Some(token) => match room.tokens.get(token) {
                Some(t) => t.seated,
                None => return Err(Refusal::BadToken),
            },
            None => None,
        };
        let sockets = room.clients.len() - usize::from(seated.is_some());
        if sockets >= self.config.max_clients_per_room
            || (rejoin.is_none() && room.occupancy() >= server.reg.capacity as usize)
        {
            return Err(Refusal::Full);
        }
        if !matches(build, &server.reg.build) || !matches(content, &server.reg.content) {
            return Err(Refusal::Mismatch);
        }
        if let Some(old) = seated {
            // Its owner came back on a new socket before this one closed.
            if let Some(old_tx) = room.clients.remove(&old) {
                let _ = old_tx.send(Out::Text(protocol::closed("replaced")));
                let _ = old_tx.send(Out::Close(CLOSE_NORMAL));
            }
            let msg = protocol::encode(&[("type", "peer_gone"), ("peer", &old.to_string())]);
            let _ = server.tx.send(Out::Text(msg));
            info!(event = "peer_loss", code = %code, server = room.server, peer = old, by = "rejoin", clients = room.clients.len(), "client replaced");
        }
        let peer = server.next_peer + 1;
        let token = rejoin.map_or_else(protocol::random_token, str::to_owned);
        // max_clients_per_room ≤ max_tokens_per_room (router()), so there is
        // always an unseated token to evict; refuse rather than overfill.
        if !issue_token(
            &mut room.tokens,
            &token,
            peer,
            generation,
            self.config.max_tokens_per_room,
        ) {
            return Err(Refusal::Full);
        }
        server.next_peer = peer;
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
        info!(event = "join", code = %code, server = room.server, peer, rejoin = rejoin.is_some(), clients = room.clients.len(), "client joined");
        Ok(Seat {
            code,
            server: room.server,
            peer,
            token,
            region: server.reg.region.clone(),
        })
    }

    /// Client → server: wraps a signaling frame in a `signal` envelope.
    /// `false` when the envelope would exceed the frame limit (§7): escaping
    /// the frame into `msg` can grow it up to sixfold, and the server's socket
    /// accepts no more than any other, so such a frame is the client's
    /// protocol error and never reaches the server.
    pub(crate) fn forward_to_server(&self, seat: &Seat, text: &str) -> bool {
        let msg = protocol::encode(&[
            ("type", "signal"),
            ("peer", &seat.peer.to_string()),
            ("msg", text),
        ]);
        if msg.len() > self.config.max_frame_bytes {
            return false;
        }
        let state = self.lock();
        let seated = state
            .rooms
            .get(&seat.code)
            .is_some_and(|r| r.server == seat.server && r.clients.contains_key(&seat.peer));
        if !seated {
            return true;
        }
        if let Some(server) = state.servers.get(&seat.server) {
            let _ = server.tx.send(Out::Text(msg));
        }
        true
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

    /// The hold of an empty room: the full TTL once it was used, else the
    /// short one (v1.3).
    fn ttl_of(&self, room: &Room) -> Duration {
        if room.used {
            self.config.room_ttl
        } else {
            self.config.unused_room_ttl.min(self.config.room_ttl)
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
        if let Some(t) = room.tokens.get_mut(&seat.token) {
            if t.seated == Some(seat.peer) {
                t.seated = None;
            }
        }
        info!(event = "peer_loss", code = %seat.code, server = seat.server, peer = seat.peer, by = "client", clients = room.clients.len(), "client left");
        if let Some(server) = servers.get(&seat.server) {
            let msg = protocol::encode(&[("type", "peer_gone"), ("peer", &seat.peer.to_string())]);
            let _ = server.tx.send(Out::Text(msg));
        }
        if room.empty() {
            room.expiry = generation;
            let ttl = self.ttl_of(room);
            self.schedule_expiry(seat.code.clone(), generation, ttl);
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
        let Some((id, tx)) = peer
            .parse::<u64>()
            .ok()
            .and_then(|p| room.clients.remove(&p).map(|tx| (p, tx)))
        else {
            debug!(server = server_id, peer, "peer_gone for an unknown peer");
            return;
        };
        // The token stays the room's: a kicked or timed-out player may rejoin.
        for t in room.tokens.values_mut() {
            if t.seated == Some(id) {
                t.seated = None;
            }
        }
        let _ = tx.send(Out::Text(protocol::closed("kicked")));
        let _ = tx.send(Out::Close(CLOSE_NORMAL));
        info!(event = "peer_loss", code = %code, server = server_id, peer, by = "server", clients = room.clients.len(), "client kicked");
        if room.empty() {
            room.expiry = generation;
            let ttl = self.ttl_of(room);
            self.schedule_expiry(code, generation, ttl);
        }
    }

    fn schedule_expiry(self: &Arc<Self>, code: String, generation: u64, ttl: Duration) {
        let lobby = Arc::clone(self);
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
            .is_some_and(|r| r.expiry == generation && r.empty());
        if !live {
            return;
        }
        self.close_room(&mut state, code, "expired");
    }

    fn schedule_unused_close(self: &Arc<Self>, code: String, id: u64) {
        let lobby = Arc::clone(self);
        let after = self.config.unused_room_ttl;
        tokio::spawn(async move {
            tokio::time::sleep(after).await;
            lobby.close_unused(&code, id);
        });
    }

    /// v1.3: a room whose server has reported no game connection within
    /// `unused_room_ttl` of its creation is released, sockets or not.
    fn close_unused(&self, code: &str, id: u64) {
        let mut state = self.lock();
        let unused = state.rooms.get(code).is_some_and(|r| r.id == id && !r.used);
        if !unused {
            return;
        }
        self.close_room(&mut state, code, "unused");
    }

    /// Ends a room on the lobby's initiative: its clients get
    /// `closed`/`room_closed`, its server `room_closed`, the code is free.
    fn close_room(&self, state: &mut State, code: &str, reason: &str) {
        let Some(room) = state.rooms.remove(code) else {
            return;
        };
        for tx in room.clients.values() {
            let _ = tx.send(Out::Text(protocol::closed("room_closed")));
            let _ = tx.send(Out::Close(CLOSE_NORMAL));
        }
        if let Some(server) = state.servers.get_mut(&room.server) {
            server.room = None;
            let msg = protocol::encode(&[("type", "room_closed"), ("code", code)]);
            let _ = server.tx.send(Out::Text(msg));
        }
        info!(event = "room_close", code = %code, server = room.server, reason, clients = room.clients.len(), "room closed");
        state.forget_room(&room);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_full_token_table_evicts_its_oldest_unseated_token() {
        let mut tokens = HashMap::new();
        assert!(issue_token(&mut tokens, "a", 1, 1, 2));
        assert!(issue_token(&mut tokens, "b", 2, 2, 2));
        // Both seated: nothing to evict.
        assert!(!issue_token(&mut tokens, "c", 3, 3, 2));
        tokens.get_mut("b").unwrap().seated = None;
        tokens.get_mut("a").unwrap().seated = None;
        assert!(issue_token(&mut tokens, "c", 3, 3, 2));
        assert!(!tokens.contains_key("a"), "the oldest unseated goes");
        assert!(tokens.contains_key("b"));
        // A token the room knows is reseated, never counted twice.
        assert!(issue_token(&mut tokens, "b", 4, 4, 2));
        assert_eq!(tokens.len(), 2);
        assert_eq!(tokens["b"].seated, Some(4));
    }
}
