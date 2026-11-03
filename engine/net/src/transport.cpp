// Compile-time checks of the transport seam; the implementations live in
// their own files (loopback.cpp, and the WebRTC transport in its own target).
#include "kfm/net/transport.hpp"

namespace kfm::net {

static_assert(kMaxUnreliableBytes < kMaxReliableBytes);
static_assert(!PeerId::invalid().isValid());
static_assert(kServerPeer.isValid());

}  // namespace kfm::net
