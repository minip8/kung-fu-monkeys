// LobbyAdapter: kfm_server's lobby mode (M6a, architecture §13.5,
// docs/lobby-protocol.md). It wires a net::LobbyLink to a ServerHost without
// ServerHost knowing about the lobby (the host stays transport-agnostic and
// loopback-tested):
//   - the host's token check is the link's token set (lobbyTokenCheck(), put
//     into HostConfig::server.acceptToken before the host is created);
//   - the stats line ends with " | lobby <status>" (lobbyStatsSuffix(), into
//     HostConfig::statsSuffix);
//   - frame() (after ServerHost::frame) polls the link, logs one line per
//     lobby event (registered, refused, room opened/closed, peer joined/gone,
//     link lost), restarts the match when a room is assigned while no peer
//     is connected (spec §6 point 5, ServerHost::startNewMatch, so the bots
//     are re-seeded) and keeps the link's capacity at max players minus the
//     host's bots (the total of human slots, not net of joined humans: the
//     lobby counts occupancy by client sockets, spec §6.2).
#pragma once

#include "kfm/net/lobby_link.hpp"
#include "server_host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace kfm::server {

// For HostConfig::server.acceptToken: accepts exactly the link's live tokens.
[[nodiscard]] std::function<bool(std::span<const std::byte>)> lobbyTokenCheck(
    const net::LobbyLink& link);
// For HostConfig::statsSuffix.
[[nodiscard]] std::function<std::string(uint64_t)> lobbyStatsSuffix(const net::LobbyLink& link);
// The human slots a process offers: maxPlayers minus its bots (at least 0).
[[nodiscard]] uint16_t lobbyCapacity(uint8_t maxPlayers, size_t bots);

class LobbyAdapter {
 public:
  // maxPlayers: the host's ServerConfig::maxPlayers.
  LobbyAdapter(ServerHost& host, net::LobbyLink& link, uint8_t maxPlayers);

  // Once per host frame, after ServerHost::frame(nowNs).
  void frame(uint64_t nowNs);

  // Rooms assigned and match restarts they caused, for tests and the summary.
  [[nodiscard]] uint32_t roomsOpened() const { return roomsOpened_; }
  [[nodiscard]] uint32_t restarts() const { return restarts_; }

 private:
  void updateCapacity();

  ServerHost& host_;
  net::LobbyLink& link_;
  uint8_t maxPlayers_{0};
  uint32_t roomsOpened_{0};
  uint32_t restarts_{0};
};

}  // namespace kfm::server
