#include "client_platform.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>

namespace kfm::client {

std::unique_ptr<platform::IPlatformServices> makeClientPlatformServices(
    const ClientPlatformConfig& config) {
  std::random_device device;
  const auto clock = static_cast<std::uint64_t>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  const std::uint64_t a = (std::uint64_t{device()} << 32) | device();
  return std::make_unique<platform::LocalPlatformServices>(makePlatformHooks(config.persist),
                                                           config.lobbyUrl, a, clock);
}

}  // namespace kfm::client
