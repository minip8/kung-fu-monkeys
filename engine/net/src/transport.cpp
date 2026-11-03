// Anchor TU for kfm_net until the first implementation lands (the loopback
// transport, the network emulator and the message layer live beside it).
#include "kfm/net/transport.hpp"

namespace kfm::net {

static_assert(kMaxUnreliableBytes < kMaxReliableBytes);
static_assert(!PeerId::invalid().isValid());
static_assert(kServerPeer.isValid());

}  // namespace kfm::net
