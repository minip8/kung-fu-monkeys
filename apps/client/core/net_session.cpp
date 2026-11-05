#include "net_session.hpp"

#include "kfm/sim/step.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <optional>
#include <utility>

namespace kfm::client {

namespace {

[[nodiscard]] constexpr sim::PlayerMask bit(uint8_t slot) {
  return static_cast<sim::PlayerMask>(1u << slot);
}

[[nodiscard]] constexpr bool has(sim::PlayerMask m, uint8_t slot) {
  return (m & bit(slot)) != 0;
}

// The k-th set bit of `m` (ascending), if any.
[[nodiscard]] std::optional<uint8_t> nthSlot(sim::PlayerMask m, uint8_t k) {
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    if (!has(m, i)) continue;
    if (k == 0) return i;
    --k;
  }
  return std::nullopt;
}

constexpr double kFrameEmaGain = 0.1;
constexpr uint64_t kRateWindowNs = 1'000'000'000ULL;

[[nodiscard]] double perSecond(uint64_t now, uint64_t before, double seconds) {
  return now >= before ? static_cast<double>(now - before) / seconds : 0.0;
}

}  // namespace

const char* netPhaseName(NetPhase phase) {
  switch (phase) {
    case NetPhase::Connecting:
      return "connecting";
    case NetPhase::Syncing:
      return "syncing";
    case NetPhase::Running:
      return "running";
    case NetPhase::Waiting:
      return "waiting";
    case NetPhase::Over:
      return "over";
    case NetPhase::Rejected:
      return "rejected";
    case NetPhase::Disconnected:
      return "disconnected";
    case NetPhase::Failed:
      return "failed";
  }
  return "?";
}

const char* rejectReasonText(net::RejectReason reason) {
  switch (reason) {
    case net::RejectReason::ProtocolMismatch:
      return "protocol version mismatch";
    case net::RejectReason::ContentMismatch:
      return "content mismatch (different game data)";
    case net::RejectReason::BuildMismatch:
      return "build mismatch (update the client)";
    case net::RejectReason::BadToken:
      return "invalid connection token";
    case net::RejectReason::MatchFull:
      return "match full";
    case net::RejectReason::ServerShuttingDown:
      return "server shutting down";
  }
  return "unknown reason";
}

const char* disconnectReasonText(net::DisconnectReason reason) {
  switch (reason) {
    case net::DisconnectReason::Quit:
      return "left the match";
    case net::DisconnectReason::Kicked:
      return "kicked by the server";
    case net::DisconnectReason::Timeout:
      return "connection lost or timed out";
    case net::DisconnectReason::ProtocolError:
      return "protocol error";
    case net::DisconnectReason::ServerShuttingDown:
      return "server shut down";
  }
  return "unknown reason";
}

std::string lobbyErrorText(std::string_view error, const LobbyTarget& target) {
  constexpr std::string_view kRefused = "lobby refused: ";
  constexpr std::string_view kClosed = "lobby closed: ";
  const std::string room = target.create ? std::string("The room") : "Room " + target.code;
  if (error.starts_with(kRefused)) {
    const std::string_view reason = error.substr(kRefused.size());
    if (reason == "unknown_code") return room + " not found (check the code)";
    if (reason == "full") return room + " is full";
    if (reason == "no_server") {
      return target.create ? "No server available in " + target.region
                           : std::string("No server available for this room");
    }
    if (reason == "mismatch") return "Version mismatch: reload the page or update the client";
    if (reason == "bad_request") return "The lobby did not understand the request";
    if (reason == "server_gone") return room + "'s server went away";
    if (reason == "bad_token") return room + " does not know this player any more";  // v1.2
    return "The lobby refused: " + std::string(reason);
  }
  if (error.starts_with(kClosed)) {
    const std::string_view reason = error.substr(kClosed.size());
    if (reason == "server_gone") return room + "'s server went away";
    if (reason == "kicked") return "Removed from the room by its server";
    if (reason == "room_closed") return room + " closed";
    if (reason == "replaced") return "Joined " + room + " again from elsewhere";  // v1.2
    return "The lobby closed the room: " + std::string(reason);
  }
  if (error == "lobby connection closed") return "Lost the connection to the lobby";
  return {};
}

NetSession::NetSession(NetSessionConfig config, platform::InputSampler lobbySampler,
                       net::ITransport* transport, uint64_t nowNs,
                       std::unique_ptr<net::ITransport> owned, std::string failure)
    : config_(std::move(config)),
      content_(config_.content != nullptr ? *config_.content : sim::defaultContent()),
      ownedTransport_(std::move(owned)),
      failure_(std::move(failure)),
      nowNs_(nowNs),
      sampler_(std::move(lobbySampler)),
      idleWorld_(sim::initialWorld(sim::MatchConfig{.seed = 1, .players = 0})),
      lastFrameNs_(nowNs),
      rateWindowStartNs_(nowNs) {
  // The lobby's devices in ascending lobby slot order become local devices
  // 0, 1, ...; they are bound to server slots once the server assigns them.
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i) {
    const PlayerId slot{i};
    if (!sampler_.isBound(slot)) continue;
    devices_.push_back(LobbyDevice{.lobbySlot = slot, .device = sampler_.binding(slot)});
    sampler_.unbind(slot);
  }
  sampler_.clearDropped();
  if (config_.spectate) {
    requested_ = 0;
  } else if (config_.requestedSlots != 0) {
    requested_ = std::min<uint8_t>(config_.requestedSlots, sim::kMaxPlayers);
  } else {
    requested_ = static_cast<uint8_t>(std::max<std::size_t>(devices_.size(), 1));
  }

  net::ClientConfig cc;
  cc.buildId = config_.buildId;
  cc.requestedSlots = requested_;
  cc.inputDelayTicks = config_.inputDelayTicks;
  cc.nickname = net::Nickname::from(config_.nickname);
  cc.setToken(config_.token);
  token_ = config_.token;
  client_ = std::make_unique<net::NetClient>(content_, cc);
  frameEvents_.resize(kMaxFrameEvents);
  if (transport == nullptr) {
    if (failure_.empty()) failure_ = "no transport";
    return;
  }
  failure_.clear();
  client_->connect(*transport, nowNs);
}

NetSession::~NetSession() {
  // The client may still send on the transport it holds: tear it down first.
  client_.reset();
  lobby_.reset();  // may look at the transport
  ownedTransport_.reset();
}

// --- per frame ----------------------------------------------------------------

void NetSession::push(const platform::InputEvent& event) {
  sampler_.push(event);
}

void NetSession::advance(uint64_t nowNs) {
  nowNs_ = nowNs;
  frameEventCount_ = 0;
  ticksThisFrame_ = 0;
  updateDisplayRate(nowNs);
  if (!failure_.empty()) {
    sampler_.endTick();
    return;
  }

  net::NetClient& c = *client_;
  // Lobby mode: the room's token goes in before the Hello (the transport
  // reports Connected only after the lobby's `joined`).
  if (lobby_ && !room_) {
    room_ = lobby_->room();
    if (room_) {
      token_ = lobby_->token();
      c.setToken(token_);
    }
  }
  c.beginFrame();
  c.pump(nowNs);
  c.reconcile();
  if (c.state() == net::ClientState::Syncing || c.running()) configKnown_ = true;
  const bool rolledBack = c.frameReport().rolledBack;
  if (rolledBack && everRan_) {
    // The re-simulated timeline ends at the tick drawn last frame: compare at
    // the same (prev, curr, alpha), so the offset is the correction alone.
    presentation::onRollback(presentation_, lastNewest_ ? c.world() : c.prevWorld(), c.world(),
                             lastAlpha_, c.localSlots());
    ++rollbackFrames_;
  }

  if (!c.running()) {
    // Connecting, syncing or over: nothing is sampled, presses never pile up.
    sampler_.endTick();
    updateRates(nowNs);
    return;
  }
  if (!everRan_) {
    everRan_ = true;
    ranBefore_ = true;
    presentation_ = presentation::PresentationState{};
  }
  // v0.44: a spectator that waited for a bot's slot binds its devices when
  // the slot arrives.
  if (!slotsBound_ || c.localSlots() != boundFor_) bindLocalSlots();
  handleRebinds();

  const uint32_t due = c.ticksDue(nowNs);
  // v0.42: the room waits or shows its match over; no presses pile up.
  if (c.holding()) sampler_.endTick();
  stepTicks(due);
  if (c.frameReport().rolledBack && !rolledBack) {
    // A hard clock jump inside ticksDue(): the timeline moved forward at once.
    presentation::onRollback(presentation_, renderPrev(), curr(), renderAlpha(), c.localSlots());
    ++rollbackFrames_;
  }

  const std::span<const sim::SimEvent> events = c.drainEvents();
  frameEventCount_ = std::min(events.size(), frameEvents_.size());
  std::copy_n(events.begin(), frameEventCount_, frameEvents_.begin());
  updateRates(nowNs);
}

void NetSession::stepTicks(uint32_t ticks) {
  net::NetClient& c = *client_;
  const uint64_t wall = c.tickWallNs();
  std::array<sim::PlayerInput, sim::kMaxPlayers> inputs{};
  for (uint32_t i = 0; i < ticks; ++i) {
    // Tick i of the batch takes the presses stamped inside its window; the
    // last one ends at nowNs and takes everything still pending.
    const uint64_t start = c.tickStartNs(i);
    sim::InputFrame frame{};
    // v0.45: aim from the predicted world (the one this tick steps), never
    // the confirmed one, or the aim would lag the monkey by the lead.
    feedMouseAim(sampler_, c.world(), c.matchContent().arena, aimViewport_);
    const sim::PlayerMask bound = sampler_.sampleFrame(
        frame, start, wall, i + 1 < ticks ? start + wall : platform::kNoCutoff);
    std::size_t k = 0;
    for (uint8_t s = 0; s < sim::kMaxPlayers; ++s) {
      if (!has(c.localSlots(), s)) continue;
      // A slot without a device (pad removed, or more slots than devices)
      // repeats its last input; held buttons cannot re-fire (§5.4).
      if (has(bound, s)) lastInputs_[s] = frame.inputs[s];
      inputs[k++] = lastInputs_[s];
    }
    c.tick(std::span<const sim::PlayerInput>(inputs.data(), k));
    ++ticksThisFrame_;
  }
}

void NetSession::bindLocalSlots() {
  slotsBound_ = true;
  const sim::PlayerMask slots = client_->localSlots();
  boundFor_ = slots;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    if (has(boundLocal_, i)) sampler_.unbind(PlayerId{i});
  boundLocal_ = 0;
  for (std::size_t d = 0; d < devices_.size(); ++d) {
    const std::optional<uint8_t> slot = nthSlot(slots, static_cast<uint8_t>(d));
    if (!slot) break;
    (void)sampler_.bind(PlayerId{*slot}, devices_[d].device);
    boundLocal_ = static_cast<sim::PlayerMask>(boundLocal_ | bit(*slot));
  }
  lastInputs_ = {};
  sampler_.endTick();
  sampler_.consumeAllButtons();  // a button held from the lobby does not fire
}

void NetSession::handleRebinds() {
  for (uint32_t guard = 0; guard < sim::kMaxPlayers; ++guard) {
    const auto dropped = static_cast<sim::PlayerMask>(boundLocal_ & ~sampler_.boundSlots());
    if (dropped == 0) break;
    const std::optional<platform::DeviceBinding> device = sampler_.pollJoin();
    if (!device) break;
    const auto slot = static_cast<uint8_t>(std::countr_zero(static_cast<uint32_t>(dropped)));
    (void)sampler_.bind(PlayerId{slot}, *device, lastInputs_[slot].aimDir);
    sampler_.consumeButtons(PlayerId{slot});  // the takeover press is not a Throw/Dash
  }
}

void NetSession::build(float dtSeconds, presentation::RenderState& out) {
  lastNewest_ = rendersNewest();
  lastAlpha_ = renderAlpha();
  presentation_.pingSlots = localSlots();  // v0.35: a local human's call pings
  presentation::buildRenderState(presentation_, renderPrev(), curr(), lastAlpha_, frameEvents(),
                                 client_->matchContent(), dtSeconds, out);
}

void NetSession::disconnect() {
  if (failure_.empty()) client_->disconnect();
}

bool NetSession::canReconnect() const {
  if (!failure_.empty() || !ranBefore_ || phase() != NetPhase::Disconnected) return false;
  const std::optional<net::DisconnectReason> r = client_->disconnectReason();
  if (r && *r != net::DisconnectReason::Timeout) return false;  // quit, kicked, shut down
  if (!lobby_) return true;
  // The lobby dropped the socket or the server dropped the peer (`kicked`
  // is how a server-side timeout reaches the client); a closed room, a gone
  // server or a replaced socket has nothing to come back to.
  const std::string e = lobby_->error();
  return e.empty() || e == "lobby closed: kicked" || e == "lobby connection closed" ||
         !e.starts_with("lobby ");
}

NetSessionConfig NetSession::reconnectConfig() const {
  NetSessionConfig c = config_;
  c.token = token_;
  if (config_.lobby && room_) {
    std::string hex;
    for (const std::byte b : token_)
      hex += static_cast<char>(b);
    c.lobby = LobbyTarget{.create = false, .region = room_->region, .code = room_->code};
    c.rejoinToken = std::move(hex);
  }
  return c;
}

void NetSession::reconnect(net::ITransport* transport, uint64_t nowNs,
                           std::unique_ptr<net::ITransport> owned, std::string failure) {
  config_ = reconnectConfig();
  ++reconnects_;
  // The lobby view looks at the old transport: gone first. The room stays
  // known (its code labels the status) and is confirmed by the rejoin.
  lobby_.reset();
  failure_ = std::move(failure);
  if (transport == nullptr) {
    // The client keeps the old (dead) transport; nothing more is sent.
    if (failure_.empty()) failure_ = "no transport";
    return;
  }
  client_->setToken(token_);
  client_->connect(*transport, nowNs);  // lets go of the old transport
  ownedTransport_ = std::move(owned);
  room_.reset();
  slotsBound_ = false;
  everRan_ = false;  // a fresh presentation when it runs again
  configKnown_ = false;
}

void NetSession::updateDisplayRate(uint64_t nowNs) {
  if (nowNs > lastFrameNs_) {
    const auto dt = static_cast<double>(nowNs - lastFrameNs_);
    frameNsEma_ = frameNsEma_ == 0.0 ? dt : frameNsEma_ + (kFrameEmaGain * (dt - frameNsEma_));
  }
  lastFrameNs_ = nowNs;
}

void NetSession::updateRates(uint64_t nowNs) {
  if (nowNs < rateWindowStartNs_ + kRateWindowNs) return;
  const double seconds = static_cast<double>(nowNs - rateWindowStartNs_) * 1e-9;
  const net::ClientNetStats& s = client_->netStats();
  const net::ClientNetStats& b = rateBase_;
  rates_.mispredictedTicksPerSec = perSecond(s.mispredictedTicks, b.mispredictedTicks, seconds);
  rates_.rollbacksPerSec = perSecond(s.rollbacks, b.rollbacks, seconds);
  rates_.substitutionsPerSec = perSecond(s.substitutionsSeen, b.substitutionsSeen, seconds);
  rates_.bytesUpPerSec = perSecond(s.bytesSent, b.bytesSent, seconds);
  rates_.bytesDownPerSec = perSecond(s.bytesReceived, b.bytesReceived, seconds);
  rates_.ticksPerSec = perSecond(s.ticks, b.ticks, seconds);
  rateBase_ = s;
  rateWindowStartNs_ = nowNs;
}

// --- status -------------------------------------------------------------------

NetPhase NetSession::phase() const {
  if (!failure_.empty()) return NetPhase::Failed;
  switch (client_->state()) {
    case net::ClientState::Idle:
    case net::ClientState::Connecting:
      return NetPhase::Connecting;
    case net::ClientState::Syncing:
      return NetPhase::Syncing;
    case net::ClientState::Running:
      if (const std::optional<net::RoomStateMsg>& room = client_->roomState()) {
        if (room->phase == net::RoomPhase::Waiting) return NetPhase::Waiting;
        if (room->phase == net::RoomPhase::Over) return NetPhase::Over;
      }
      return NetPhase::Running;
    case net::ClientState::Rejected:
      return NetPhase::Rejected;
    case net::ClientState::Disconnected:
      return NetPhase::Disconnected;
  }
  return NetPhase::Disconnected;
}

std::string NetSession::lobbyErrorText() const {
  if (!lobby_ || !config_.lobby) return {};
  LobbyTarget target = *config_.lobby;
  if (room_) {  // a created room has its code now
    target.create = false;
    target.code = room_->code;
  }
  return client::lobbyErrorText(lobby_->error(), target);
}

std::string NetSession::statusText() const {
  char buf[256];
  std::string label = config_.serverLabel.empty() ? "the server" : config_.serverLabel;
  if (room_) label = "room " + room_->code + " (" + room_->region + ")";
  const char* server = label.c_str();
  const NetPhase ph = phase();
  if (config_.lobby && (ph == NetPhase::Disconnected || ph == NetPhase::Rejected)) {
    std::string why = lobbyErrorText();
    if (!why.empty()) return why;
    const std::optional<net::DisconnectReason> r = client_->disconnectReason();
    if (!room_ && ph == NetPhase::Disconnected && r != net::DisconnectReason::Quit) {
      return "Could not reach the lobby at " +
             (config_.serverLabel.empty() ? std::string("its URL") : config_.serverLabel);
    }
  }
  switch (ph) {
    case NetPhase::Connecting:
      if (config_.lobby && !room_) {
        if (config_.lobby->create) {
          std::snprintf(buf, sizeof buf, "Creating a room in %s ...",
                        config_.lobby->region.c_str());
        } else {
          std::snprintf(buf, sizeof buf, "Joining room %s ...", config_.lobby->code.c_str());
        }
        break;
      }
      std::snprintf(buf, sizeof buf, "Connecting to %s ...", server);
      break;
    case NetPhase::Syncing:
      std::snprintf(buf, sizeof buf, "Syncing with %s ...", server);
      break;
    case NetPhase::Waiting:
    case NetPhase::Over: {
      const bool host = isHost();
      const char* what = nullptr;
      if (ph == NetPhase::Waiting) {
        what =
            host ? "you are the host: pick the match and start" : "waiting for the host to start";
      } else {
        what = host ? "match over: play again when ready" : "match over: waiting for the host";
      }
      std::snprintf(buf, sizeof buf, "In %s, %s", server, what);
      break;
    }
    case NetPhase::Running: {
      const auto local = std::popcount(static_cast<uint32_t>(localSlots()));
      if (local == 0 && client_->waitingForSlot()) {
        // v0.44: queued until a bot gives its slot up.
        std::snprintf(buf, sizeof buf, "Spectating on %s until a bot's slot is free, RTT %.0f ms",
                      server, static_cast<double>(client_->netStats().rttNs) * 1e-6);
      } else if (local == 0) {
        std::snprintf(buf, sizeof buf, "Spectating on %s, RTT %.0f ms", server,
                      static_cast<double>(client_->netStats().rttNs) * 1e-6);
      } else {
        std::snprintf(buf, sizeof buf, "Online on %s, %d local slot%s, RTT %.0f ms", server, local,
                      local == 1 ? "" : "s", static_cast<double>(client_->netStats().rttNs) * 1e-6);
      }
      break;
    }
    case NetPhase::Rejected:
      std::snprintf(buf, sizeof buf, "Rejected by %s: %s", server,
                    rejectReasonText(client_->rejectReason()));
      break;
    case NetPhase::Disconnected: {
      const std::optional<net::DisconnectReason> r = client_->disconnectReason();
      if (!everRan_ && client_->state() == net::ClientState::Disconnected &&
          (!r || *r == net::DisconnectReason::Timeout)) {
        std::snprintf(buf, sizeof buf, "Disconnected: could not reach %s", server);
      } else {
        std::snprintf(buf, sizeof buf, "Disconnected from %s: %s", server,
                      r ? disconnectReasonText(*r) : "connection closed");
      }
      break;
    }
    case NetPhase::Failed:
      std::snprintf(buf, sizeof buf, "Cannot connect to %s: %s", server, failure_.c_str());
      break;
  }
  return buf;
}

// --- the room -------------------------------------------------------------------

bool NetSession::isHost() const {
  const std::optional<net::RoomStateMsg>& room = client_->roomState();
  return room && room->host.isValid() && sim::maskHas(client_->localSlots(), room->host);
}

void NetSession::sendSettings(const net::RoomSettings& settings) {
  if (failure_.empty()) client_->sendRoomSettings(settings);
}

void NetSession::start() {
  if (failure_.empty()) client_->sendStart();
}

std::string NetSession::slotName(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return {};
  const std::optional<net::RoomStateMsg>& room = client_->roomState();
  if (room && sim::maskHas(room->humans, slot) && !room->names[slot.value].empty()) {
    return std::string(room->names[slot.value].view());
  }
  const bool bot = room && sim::maskHas(room->bots, slot);
  return (bot ? "BOT" : "P") + std::to_string(slot.value + 1);
}

// --- slots --------------------------------------------------------------------

PlayerId NetSession::slotOfDevice(uint8_t device) const {
  if (device >= devices_.size()) return PlayerId::invalid();
  const std::optional<uint8_t> slot = nthSlot(client_->localSlots(), device);
  return slot ? PlayerId{*slot} : PlayerId::invalid();
}

SlotOwner NetSession::owner(PlayerId slot) const {
  if (!slot.isValid() || slot.value >= sim::kMaxPlayers) return SlotOwner::None;
  const uint8_t i = slot.value;
  if (has(client_->localSlots(), i)) {
    return has(boundLocal_, i) && !sampler_.isBound(slot) ? SlotOwner::Dropped : SlotOwner::Local;
  }
  return has(curr().rules.present, i) ? SlotOwner::Remote : SlotOwner::None;
}

sim::PlayerMask NetSession::droppedMask() const {
  return static_cast<sim::PlayerMask>(boundLocal_ & ~sampler_.boundSlots());
}

// --- state --------------------------------------------------------------------

const sim::WorldState& NetSession::curr() const {
  return everRan_ ? client_->world() : idleWorld_;
}

bool NetSession::rendersNewest() const {
  switch (policy_) {
    case InterpolationPolicy::Interpolate:
      return false;
    case InterpolationPolicy::Newest:
      return true;
    case InterpolationPolicy::Auto:
      break;
  }
  return displayHz() >= static_cast<double>(sim::kTickHz) * kNewestTickRateTolerance;
}

const sim::WorldState& NetSession::renderPrev() const {
  if (!everRan_ || rendersNewest()) return curr();
  return client_->prevWorld();
}

float NetSession::renderAlpha() const {
  if (!everRan_ || rendersNewest()) return 1.0f;
  return static_cast<float>(client_->renderPhaseQ16()) / 65536.0f;
}

double NetSession::displayHz() const {
  return frameNsEma_ > 0.0 ? 1e9 / frameNsEma_ : 0.0;
}

uint32_t NetSession::checksum() const {
  return sim::checksum(curr());
}

platform::InputSampler NetSession::lobbySampler() const {
  platform::InputSampler s = sampler_;
  for (uint8_t i = 0; i < sim::kMaxPlayers; ++i)
    s.unbind(PlayerId{i});
  for (const LobbyDevice& d : devices_) {
    // A pad that was removed in the meantime stays unbound.
    if (d.device.kind == platform::DeviceKind::Gamepad && !s.isPadConnected(d.device.padId)) {
      continue;
    }
    (void)s.bind(d.lobbySlot, d.device);
  }
  s.clearDropped();
  s.endTick();
  s.consumeAllButtons();
  return s;
}

}  // namespace kfm::client
