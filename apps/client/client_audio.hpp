// The client's sound (architecture §7 "Audio"): the procedural SoundBank, the
// Mixer and the platform AudioDevice, driven once per frame on the main
// thread. Settings come from --mute / --volume and the F1 overlay's Audio
// panel; the lobby UI can call the same setters and click().
//
// Device policy: natively the device opens on the first frame that has sound
// enabled; in the browser on the first input event after that (autoplay
// policy, §13). A device that fails to open stays closed for the session and
// the client is silent; nothing else changes.
#pragma once

#include "kfm/platform/audio_device.hpp"
#include "kfm/presentation/mixer.hpp"
#include "kfm/presentation/render_state.hpp"
#include "kfm/presentation/sound_bank.hpp"

#include <cstdint>
#include <string>

namespace kfm::client {

class ClientAudio {
 public:
  // Synthesises the sound bank (allocates; call once at startup).
  void init(bool muted, uint8_t volumePercent);
  // Releases the device; call before SDL_Quit (Window::shutdown).
  void shutdown() { device_.close(); }

  // A key, mouse button, touch or gamepad button: allows the browser to open
  // the device.
  void onUserGesture() { gestureSeen_ = true; }

  // Plays this frame's cues (RenderState::sounds) and tops the device up.
  void frame(const presentation::RenderState& state);
  // A UI click (lobby buttons, joins); also a gesture.
  void click();

  void setMuted(bool muted) { mixer_.setMuted(muted); }
  [[nodiscard]] bool muted() const { return mixer_.muted(); }
  // 0..100, the master volume in percent.
  void setVolumePercent(int percent);
  [[nodiscard]] int volumePercent() const;

  [[nodiscard]] const presentation::Mixer& mixer() const { return mixer_; }
  [[nodiscard]] const platform::AudioDevice& device() const { return device_; }
  // One line for the overlay: device status, or why it is not open yet.
  [[nodiscard]] std::string statusText() const;

 private:
  void ensureOpen();

  presentation::SoundBank bank_;
  presentation::Mixer mixer_{&bank_};
  platform::AudioDevice device_;
#ifdef __EMSCRIPTEN__
  bool gestureSeen_{false};
#else
  bool gestureSeen_{true};
#endif
};

}  // namespace kfm::client
