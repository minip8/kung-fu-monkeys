// SoundBank (architecture §7 "Audio"): every game sound synthesised at
// startup from sines, noise and envelopes, so no audio asset ships and the
// browser download does not grow. Clips are mono float in [-1, 1] at the
// bank's sample rate; generation is deterministic (a fixed-seed noise
// generator) and allocates once, in generate(). SDL-free and headless-tested.
#pragma once

#include "kfm/presentation/sound.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace kfm::presentation {

struct SoundClip {
  std::vector<float> samples;  // mono, peak <= kClipPeak
  float gain{1.0f};            // per-clip mix gain, multiplied with the cue's
};

class SoundBank {
 public:
  // Peak amplitude every clip is normalised to: room for a few overlapping
  // voices before the mixer's limiter has to act.
  static constexpr float kClipPeak = 0.8f;

  // Synthesises every clip at sampleRate (8000..192000 Hz). Allocates.
  void generate(int sampleRate);

  [[nodiscard]] bool ready() const { return sampleRate_ > 0; }
  [[nodiscard]] int sampleRate() const { return sampleRate_; }
  [[nodiscard]] const SoundClip& clip(SoundId id) const {
    return clips_[static_cast<uint8_t>(id) % kSoundCount];
  }
  [[nodiscard]] std::span<const float> samples(SoundId id) const { return clip(id).samples; }
  [[nodiscard]] float seconds(SoundId id) const {
    return sampleRate_ > 0
               ? static_cast<float>(clip(id).samples.size()) / static_cast<float>(sampleRate_)
               : 0.0f;
  }

 private:
  std::array<SoundClip, kSoundCount> clips_{};
  int sampleRate_{0};
};

// Clip lengths in seconds (the synthesiser writes exactly round(rate × this)
// samples per clip).
[[nodiscard]] float soundLengthSeconds(SoundId id);

}  // namespace kfm::presentation
