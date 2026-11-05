#include "server_lobby.hpp"

#include "kfm/net/lobby_link.hpp"
#include "server_host.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace kfm::server {

std::function<bool(std::span<const std::byte>)> lobbyTokenCheck(const net::LobbyLink& link) {
  return [&link](std::span<const std::byte> token) { return link.acceptsToken(token); };
}

std::function<std::string(uint64_t)> lobbyStatsSuffix(const net::LobbyLink& link) {
  return [&link](uint64_t nowNs) {
    return std::format(" | lobby {} reconnects={}", link.statusText(nowNs), link.reconnects());
  };
}

uint16_t lobbyCapacity(uint8_t maxPlayers, size_t bots) {
  return bots >= maxPlayers ? uint16_t{0} : static_cast<uint16_t>(maxPlayers - bots);
}

LobbyAdapter::LobbyAdapter(ServerHost& host, net::LobbyLink& link, uint8_t maxPlayers)
    : host_(host), link_(link), maxPlayers_(maxPlayers) {
  updateCapacity();
}

void LobbyAdapter::updateCapacity() {
  // v0.44: the room flow's bots yield to joining humans, so every slot is a
  // human's to take; the M3 flow's bots keep theirs.
  const auto bots = host_.room() != nullptr
                        ? size_t{0}
                        : static_cast<size_t>(std::popcount(uint32_t{host_.server().bots()}));
  // The link sends `capacity` only when this total changes.
  link_.setCapacity(lobbyCapacity(maxPlayers_, bots));
}

void LobbyAdapter::frame(uint64_t nowNs) {
  updateCapacity();
  for (const net::LobbyEvent& e : link_.poll(nowNs)) {
    switch (e.kind) {
      case net::LobbyEventKind::Registered:
        host_.logLine(
            std::format("lobby registered at {} capacity={}", link_.url(), link_.capacity()));
        break;
      case net::LobbyEventKind::Refused:
        host_.logLine(std::format("lobby refused the registration: {}", e.text));
        break;
      case net::LobbyEventKind::RoomAssigned: {
        ++roomsOpened_;
        const bool empty = host_.server().peers().empty();
        host_.logLine(std::format("lobby room {} opened{}", e.text,
                                  empty ? "" : " (peers still connected: match continues)"));
        // A new group starts a fresh match with the server's bots (§6.5),
        // or (v0.42, the room flow) a fresh waiting room with its defaults.
        if (empty) {
          ++restarts_;
          host_.openRoom(std::format("room {}", e.text));
        }
        break;
      }
      case net::LobbyEventKind::RoomClosed:
        host_.logLine(std::format("lobby room {} closed", e.text));
        break;
      case net::LobbyEventKind::PeerJoined:
        host_.logLine(std::format("lobby peer {} announced as peer={}", e.lobbyPeer, e.peer.value));
        break;
      case net::LobbyEventKind::PeerGone:
        host_.logLine(std::format("lobby peer {} (peer={}) gone: {}", e.lobbyPeer, e.peer.value,
                                  e.byServer ? "dropped by the server" : "left the lobby"));
        break;
      case net::LobbyEventKind::LinkLost:
        host_.logLine(std::format("lobby link lost: {}; retry in {} ms", e.text, e.retryInMs));
        break;
    }
  }
}

}  // namespace kfm::server
