// Mixer (architecture §7 "Audio"): a fixed number of voices playing
// SoundBank clips into interleaved stereo float. Pull API: the platform's
// AudioDevice calls mixInto() on the main thread each frame to top up its
// queue, so the client stays single-threaded and nothing here locks.
//
// - play() starts a clip on a free voice; when all kMixerVoices are busy it
//   steals the voice that has played longest (the one nearest its end is
//   least audible in practice, and the oldest is what players stop noticing).
// - gain = cue gain × clip gain × master volume; pan is constant-power, -1
//   left .. +1 right.
// - muted or volume 0 writes silence (voices still advance, so unmuting
//   does not replay stale sounds).
// - the sum is hard-limited to [-1, 1].
// No allocation after construction; SDL-free and headless-tested.
#pragma once

#include "kfm/presentation/sound.hpp"
#include "kfm/presentation/sound_bank.hpp"
#include "kfm/presentation/tuning.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kfm::presentation {

class Mixer {
 public:
  explicit Mixer(const SoundBank* bank = nullptr) : bank_(bank) {}
  void setBank(const SoundBank* bank) {
    bank_ = bank;
    stopAll();
  }

  // Starts a clip. Returns the voice index used, or -1 without a bank.
  int play(SoundId id, float gain = 1.0f, float pan = 0.0f);
  void play(const SoundCue& cue) { (void)play(cue.id, cue.gain, cue.pan); }
  void stopAll();

  // Writes `frames` stereo frames (2 × frames floats, L R L R ...) into out,
  // overwriting it, and advances every voice.
  void mixInto(float* interleaved, std::size_t frames);

  void setMasterVolume(float volume) { volume_ = clamp01(volume); }
  [[nodiscard]] float masterVolume() const { return volume_; }
  void setMuted(bool muted) { muted_ = muted; }
  [[nodiscard]] bool muted() const { return muted_; }
  [[nodiscard]] bool silent() const { return muted_ || volume_ <= 0.0f; }

  [[nodiscard]] int activeVoices() const;
  [[nodiscard]] uint64_t stolenVoices() const { return stolen_; }
  [[nodiscard]] uint64_t started() const { return started_; }

 private:
  struct Voice {
    bool active{false};
    SoundId id{SoundId::UiClick};
    std::size_t position{0};  // next sample of the clip
    float left{0.0f};         // per-channel gain (cue × clip × pan law)
    float right{0.0f};
    uint64_t serial{0};  // start order, for stealing
  };

  const SoundBank* bank_{nullptr};
  std::array<Voice, kMixerVoices> voices_{};
  float volume_{kDefaultMasterVolume};
  bool muted_{false};
  uint64_t serial_{0};
  uint64_t stolen_{0};
  uint64_t started_{0};
};

}  // namespace kfm::presentation
