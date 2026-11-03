#include "kfm/platform/audio_device.hpp"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>

namespace kfm::platform {

namespace {
constexpr int kBytesPerFrame = 2 * static_cast<int>(sizeof(float));
}  // namespace

bool AudioDevice::open(int sampleRate) {
  if (stream_ != nullptr) return true;
  if (attempted_) return false;
  attempted_ = true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    status_ = std::string("silent: no audio driver (") + SDL_GetError() + ")";
    return false;
  }
  subsystem_ = true;
  const SDL_AudioSpec spec{.format = SDL_AUDIO_F32, .channels = 2, .freq = sampleRate};
  // SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK spelled with a C++ cast: the SDL macro
  // uses a C cast, which -Wold-style-cast rejects where the vcpkg headers are
  // not system includes.
  constexpr auto kDefaultPlayback = static_cast<SDL_AudioDeviceID>(0xFFFFFFFFu);
  static_assert(kDefaultPlayback == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
  stream_ = SDL_OpenAudioDeviceStream(kDefaultPlayback, &spec, nullptr, nullptr);
  if (stream_ == nullptr) {
    status_ = std::string("silent: no audio device (") + SDL_GetError() + ")";
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    subsystem_ = false;
    return false;
  }
  // Device streams start paused.
  if (!SDL_ResumeAudioStreamDevice(stream_)) {
    status_ = std::string("silent: the audio device did not start (") + SDL_GetError() + ")";
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    subsystem_ = false;
    return false;
  }
  sampleRate_ = sampleRate;
  const char* driver = SDL_GetCurrentAudioDriver();
  status_ = std::string("open: ") + (driver != nullptr ? driver : "?") + ", " +
            std::to_string(sampleRate) + " Hz";
  return true;
}

void AudioDevice::close() {
  if (stream_ != nullptr) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  primed_ = false;
  if (subsystem_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    subsystem_ = false;
  }
  if (attempted_ && status_.starts_with("open")) status_ = "closed";
}

std::size_t AudioDevice::queuedFrames() const {
  if (stream_ == nullptr) return 0;
  const int bytes = SDL_GetAudioStreamQueued(stream_);
  return bytes > 0 ? static_cast<std::size_t>(bytes / kBytesPerFrame) : 0;
}

void AudioDevice::pump(AudioPullFn pull, void* user, float queueSeconds) {
  if (stream_ == nullptr || pull == nullptr) return;
  const auto target = static_cast<std::size_t>(
      std::lround(std::max(queueSeconds, 0.0f) * static_cast<float>(sampleRate_)));
  std::size_t queued = queuedFrames();
  if (queued == 0 && primed_) ++underruns_;  // the device drained everything: a gap was heard
  primed_ = true;
  while (queued < target) {
    const std::size_t frames = std::min(kChunkFrames, target - queued);
    pull(user, scratch_.data(), frames);
    if (!SDL_PutAudioStreamData(stream_, scratch_.data(),
                                static_cast<int>(frames) * kBytesPerFrame)) {
      return;
    }
    queued += frames;
  }
}

}  // namespace kfm::platform
