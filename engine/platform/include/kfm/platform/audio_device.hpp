// AudioDevice (architecture §7 "Audio", §13): the default playback device as
// an SDL3 audio stream, float32 stereo at the mixer's rate (SDL converts to
// whatever the device runs at).
//
// - Opened lazily by open(), which initialises SDL's audio subsystem on first
//   use. No device, no driver, or any failure leaves it closed and silent:
//   never an error, and open() is attempted once (failed() then stays true),
//   so CI, xvfb and the headless smoke test simply run without sound.
// - Single-threaded: no SDL audio callback. pump() runs on the main thread
//   every frame and tops the stream's queue up to `queueSeconds` of audio by
//   pulling from the mixer; SDL's own device thread (natively) or the Web
//   Audio callback (browser) only drains the queue.
// - Browser: an AudioContext may only start after a user gesture, so the
//   client opens the device on the first input event; SDL resumes the
//   context once navigator.userActivation says the page was interacted with.
// close() must run before SDL_Quit (Window::shutdown).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct SDL_AudioStream;

namespace kfm::platform {

// Fills `frames` interleaved stereo frames (2 × frames floats) at `out`.
using AudioPullFn = void (*)(void* user, float* out, std::size_t frames);

class AudioDevice {
 public:
  AudioDevice() = default;
  AudioDevice(const AudioDevice&) = delete;
  AudioDevice& operator=(const AudioDevice&) = delete;
  ~AudioDevice() { close(); }

  // Opens the default playback device at sampleRate Hz. Returns isOpen().
  bool open(int sampleRate);
  void close();

  [[nodiscard]] bool isOpen() const { return stream_ != nullptr; }
  [[nodiscard]] bool attempted() const { return attempted_; }
  [[nodiscard]] bool failed() const { return attempted_ && stream_ == nullptr; }
  // "closed", "open: <driver> <rate> Hz" or why it is silent.
  [[nodiscard]] const std::string& status() const { return status_; }
  [[nodiscard]] int sampleRate() const { return sampleRate_; }

  // Pulls audio into the stream until about queueSeconds are queued.
  void pump(AudioPullFn pull, void* user, float queueSeconds);
  // Frames waiting in the stream (0 when closed).
  [[nodiscard]] std::size_t queuedFrames() const;
  [[nodiscard]] uint64_t underruns() const { return underruns_; }

 private:
  static constexpr std::size_t kChunkFrames = 512;

  SDL_AudioStream* stream_{nullptr};
  bool attempted_{false};
  bool subsystem_{false};
  bool primed_{false};  // pumped at least once since open
  int sampleRate_{0};
  uint64_t underruns_{0};
  std::string status_{"closed"};
  std::array<float, kChunkFrames * 2> scratch_{};
};

}  // namespace kfm::platform
