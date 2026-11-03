#include "kfm/presentation/mixer.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace kfm::presentation {

int Mixer::play(SoundId id, float gain, float pan) {
  if (bank_ == nullptr || !bank_->ready() || bank_->samples(id).empty()) return -1;
  int slot = -1;
  for (std::size_t i = 0; i < voices_.size(); ++i) {
    if (!voices_[i].active) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    // All busy: steal the oldest voice.
    std::size_t oldest = 0;
    for (std::size_t i = 1; i < voices_.size(); ++i) {
      if (voices_[i].serial < voices_[oldest].serial) oldest = i;
    }
    slot = static_cast<int>(oldest);
    ++stolen_;
  }
  // Constant-power pan: angle 0 (left) .. pi/2 (right).
  const float p = std::clamp(pan, -1.0f, 1.0f);
  const float angle = (p + 1.0f) * 0.25f * kPi;
  const float g = std::max(gain, 0.0f) * bank_->clip(id).gain;
  Voice& v = voices_[static_cast<std::size_t>(slot)];
  v = Voice{.active = true,
            .id = id,
            .position = 0,
            .left = g * std::cos(angle),
            .right = g * std::sin(angle),
            .serial = ++serial_};
  ++started_;
  return slot;
}

void Mixer::stopAll() {
  for (Voice& v : voices_)
    v.active = false;
}

int Mixer::activeVoices() const {
  return static_cast<int>(std::ranges::count_if(voices_, [](const Voice& v) { return v.active; }));
}

void Mixer::mixInto(float* interleaved, std::size_t frames) {
  if (interleaved == nullptr || frames == 0) return;
  const std::span<float> out(interleaved, frames * 2);
  std::ranges::fill(out, 0.0f);
  const float master = silent() ? 0.0f : volume_;
  for (Voice& v : voices_) {
    if (!v.active || bank_ == nullptr) continue;
    const std::span<const float> clip = bank_->samples(v.id);
    const std::size_t n = std::min(frames, clip.size() - std::min(v.position, clip.size()));
    if (master > 0.0f) {
      const float l = v.left * master;
      const float r = v.right * master;
      for (std::size_t i = 0; i < n; ++i) {
        const float s = clip[v.position + i];
        out[2 * i] += s * l;
        out[(2 * i) + 1] += s * r;
      }
    }
    v.position += n;
    if (v.position >= clip.size()) v.active = false;
  }
  for (float& s : out)
    s = std::clamp(s, -1.0f, 1.0f);
}

}  // namespace kfm::presentation
