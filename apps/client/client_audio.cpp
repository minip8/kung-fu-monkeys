#include "client_audio.hpp"

#include "kfm/presentation/tuning.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kfm::client {

namespace {

void pullFromMixer(void* user, float* out, std::size_t frames) {
  static_cast<presentation::Mixer*>(user)->mixInto(out, frames);
}

}  // namespace

void ClientAudio::init(bool muted, uint8_t volumePercent) {
  bank_.generate(presentation::kAudioSampleRate);
  mixer_.setBank(&bank_);
  mixer_.setMuted(muted);
  setVolumePercent(volumePercent);
}

void ClientAudio::setVolumePercent(int percent) {
  mixer_.setMasterVolume(static_cast<float>(std::clamp(percent, 0, 100)) / 100.0f);
}

int ClientAudio::volumePercent() const {
  return static_cast<int>(std::lround(mixer_.masterVolume() * 100.0f));
}

void ClientAudio::ensureOpen() {
  if (device_.isOpen() || device_.attempted() || !gestureSeen_ || mixer_.silent()) return;
  if (device_.open(presentation::kAudioSampleRate)) {
    std::printf("kfm_client: audio %s\n", device_.status().c_str());
  } else {
    std::printf("kfm_client: %s\n", device_.status().c_str());
  }
  std::fflush(stdout);
}

void ClientAudio::frame(const presentation::RenderState& state) {
  ensureOpen();
  if (!device_.isOpen()) return;  // nothing to hear: start no voices
  for (uint16_t i = 0; i < state.soundCount; ++i)
    mixer_.play(state.sounds[i]);
  device_.pump(pullFromMixer, &mixer_, presentation::kAudioQueueSeconds);
}

void ClientAudio::click() {
  gestureSeen_ = true;
  ensureOpen();
  if (device_.isOpen()) (void)mixer_.play(presentation::SoundId::UiClick);
}

std::string ClientAudio::statusText() const {
  if (device_.attempted()) return device_.status();
  if (mixer_.silent()) return "closed (muted)";
  if (!gestureSeen_) return "waiting for the first input (browser autoplay policy)";
  return "closed";
}

}  // namespace kfm::client
