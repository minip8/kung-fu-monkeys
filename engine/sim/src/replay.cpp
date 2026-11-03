#include "kfm/sim/replay.hpp"

#include "kfm/sim/visit.hpp"

namespace kfm::sim {
namespace {
constexpr uint8_t kMagic[4] = {'K', 'F', 'M', 'R'};
}

void writeReplayHeader(BitWriter& w, const ReplayHeader& h) {
  for (const uint8_t c : kMagic)
    w.writeU8(c);
  w.writeU16(kReplayVersion);
  w.writeU64(h.seed);
  w.writeU32(h.contentHash);
  w.writeU32(h.buildHash);
  w.writeU16(h.players);
  w.writeU8(h.arena);
  w.writeU8(h.mode);
  w.writeU16(h.powerUps);
  w.writeU32(h.tickCount);
}

bool readReplayHeader(BitReader& r, ReplayHeader& h) {
  for (const uint8_t c : kMagic) {
    if (r.readU8() != c) return false;
  }
  if (r.readU16() != kReplayVersion) return false;
  h.seed = r.readU64();
  h.contentHash = r.readU32();
  h.buildHash = r.readU32();
  h.players = r.readU16();
  h.arena = r.readU8();
  h.mode = r.readU8();
  h.powerUps = r.readU16();
  h.tickCount = r.readU32();
  return r.ok();
}

void writeReplayFrame(BitWriter& w, const InputFrame& f) {
  writeTo(w, f);
}

bool readReplayFrame(BitReader& r, InputFrame& f) {
  return readFrom(r, f);
}

}  // namespace kfm::sim
