// The procedural SoundBank and the Mixer (architecture §7 "Audio"), headless:
// clip lengths and ranges, determinism, gain and pan, voice stealing, mute and
// the limiter.
#include "kfm/presentation/mixer.hpp"
#include "kfm/presentation/sound_bank.hpp"
#include "kfm/presentation/tuning.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace kfm::presentation;
using Catch::Approx;

namespace {

const SoundBank& bank48k() {
  static const SoundBank bank = [] {
    SoundBank b;
    b.generate(kAudioSampleRate);
    return b;
  }();
  return bank;
}

float peakOf(const std::vector<float>& s) {
  float m = 0.0f;
  for (const float v : s)
    m = std::max(m, std::fabs(v));
  return m;
}

// Mixes `frames` frames and returns the interleaved buffer.
std::vector<float> mix(Mixer& m, std::size_t frames) {
  std::vector<float> out(frames * 2, 123.0f);  // mixInto must overwrite
  m.mixInto(out.data(), frames);
  return out;
}

}  // namespace

TEST_CASE("every clip has its length, is finite, audible and within the peak",
          "[presentation][audio]") {
  const SoundBank& b = bank48k();
  REQUIRE(b.ready());
  CHECK(b.sampleRate() == kAudioSampleRate);
  for (uint8_t i = 0; i < kSoundCount; ++i) {
    const auto id = static_cast<SoundId>(i);
    INFO(soundName(id));
    const SoundClip& c = b.clip(id);
    const auto expected =
        static_cast<std::size_t>(std::lround(soundLengthSeconds(id) * kAudioSampleRate));
    CHECK(c.samples.size() == expected);
    CHECK(b.seconds(id) == Approx(soundLengthSeconds(id)).margin(1e-4));
    CHECK(std::ranges::all_of(c.samples, [](float v) { return std::isfinite(v); }));
    const float peak = peakOf(c.samples);
    CHECK(peak <= SoundBank::kClipPeak + 1e-5f);
    CHECK(peak > 0.5f * SoundBank::kClipPeak);  // normalised, not silent
    // Faded at both ends: no click at the start or the end.
    CHECK(std::fabs(c.samples.front()) < 1e-3f);
    CHECK(std::fabs(c.samples.back()) < 1e-3f);
    CHECK(c.gain > 0.0f);
    CHECK(c.gain <= 1.0f);
  }
  // The sounds that must read apart are different lengths and shapes.
  CHECK(b.seconds(SoundId::CatchPerfect) > 3.0f * b.seconds(SoundId::CatchAuto));
  CHECK(b.seconds(SoundId::MatchEnd) > b.seconds(SoundId::RoundEnd));
}

TEST_CASE("synthesis is deterministic and follows the sample rate", "[presentation][audio]") {
  SoundBank a;
  SoundBank b;
  a.generate(kAudioSampleRate);
  b.generate(kAudioSampleRate);
  for (uint8_t i = 0; i < kSoundCount; ++i) {
    CHECK(a.clip(static_cast<SoundId>(i)).samples == b.clip(static_cast<SoundId>(i)).samples);
  }
  SoundBank low;
  low.generate(22050);
  CHECK(low.samples(SoundId::Hit).size() ==
        static_cast<std::size_t>(std::lround(soundLengthSeconds(SoundId::Hit) * 22050.0f)));
}

TEST_CASE("the mixer applies gain, clip gain, master volume and pan", "[presentation][audio]") {
  const SoundBank& b = bank48k();
  Mixer m(&b);
  m.setMasterVolume(0.5f);
  const SoundId id = SoundId::RoundGo;  // a steady tone: easy to compare
  const std::vector<float>& clip = b.clip(id).samples;
  const float clipGain = b.clip(id).gain;

  REQUIRE(m.play(id, 1.0f, 0.0f) >= 0);
  CHECK(m.activeVoices() == 1);
  std::vector<float> out = mix(m, 2000);
  const float centre = std::cos(0.25f * kPi);  // constant-power pan at 0
  for (std::size_t i = 0; i < 2000; i += 97) {
    CHECK(out[2 * i] == Approx(clip[i] * clipGain * 0.5f * centre).margin(1e-6));
    CHECK(out[(2 * i) + 1] == Approx(out[2 * i]).margin(1e-6));
  }

  // Hard left: nothing on the right; quieter cue gain scales linearly.
  m.stopAll();
  REQUIRE(m.play(id, 0.5f, -1.0f) >= 0);
  out = mix(m, 2000);
  for (std::size_t i = 0; i < 2000; i += 97) {
    CHECK(out[2 * i] == Approx(clip[i] * clipGain * 0.5f * 0.5f).margin(1e-6));
    CHECK(std::fabs(out[(2 * i) + 1]) < 1e-7f);
  }

  // A voice ends with its clip.
  m.stopAll();
  REQUIRE(m.play(SoundId::UiClick) >= 0);
  (void)mix(m, b.samples(SoundId::UiClick).size() + 10);
  CHECK(m.activeVoices() == 0);
  out = mix(m, 64);
  CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0f; }));
}

TEST_CASE("muted or zero volume is silent but voices still advance", "[presentation][audio]") {
  const SoundBank& b = bank48k();
  Mixer m(&b);
  m.setMuted(true);
  CHECK(m.silent());
  REQUIRE(m.play(SoundId::Hit) >= 0);
  std::vector<float> out = mix(m, 1000);
  CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0f; }));
  CHECK(m.activeVoices() == 1);
  (void)mix(m, b.samples(SoundId::Hit).size());
  CHECK(m.activeVoices() == 0);  // unmuting later does not replay it

  m.setMuted(false);
  m.setMasterVolume(0.0f);
  CHECK(m.silent());
  REQUIRE(m.play(SoundId::Hit) >= 0);
  out = mix(m, 1000);
  CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0f; }));
  m.setMasterVolume(2.0f);  // clamped
  CHECK(m.masterVolume() == 1.0f);
}

TEST_CASE("the mixer steals the oldest voice when all are busy", "[presentation][audio]") {
  const SoundBank& b = bank48k();
  Mixer m(&b);
  std::vector<int> voices;
  voices.reserve(kMixerVoices);
  for (int i = 0; i < kMixerVoices; ++i)
    voices.push_back(m.play(SoundId::MatchEnd));
  CHECK(m.activeVoices() == kMixerVoices);
  CHECK(m.stolenVoices() == 0);
  std::ranges::sort(voices);
  CHECK(std::ranges::adjacent_find(voices) == voices.end());  // all distinct
  // The next sound replaces the first one started, then the second.
  CHECK(m.play(SoundId::Hit) == voices.front());
  CHECK(m.play(SoundId::Hit) == voices[1]);
  CHECK(m.stolenVoices() == 2);
  CHECK(m.activeVoices() == kMixerVoices);
  CHECK(m.started() == static_cast<uint64_t>(kMixerVoices) + 2);
}

TEST_CASE("the mix never leaves [-1, 1]", "[presentation][audio]") {
  const SoundBank& b = bank48k();
  Mixer m(&b);
  m.setMasterVolume(1.0f);
  for (int i = 0; i < kMixerVoices; ++i)
    (void)m.play(SoundId::Elimination, 1.75f, 0.0f);  // far over full scale summed
  const std::vector<float> out = mix(m, 8000);
  CHECK(std::ranges::all_of(out, [](float v) { return std::isfinite(v) && std::fabs(v) <= 1.0f; }));
  CHECK(std::ranges::any_of(out, [](float v) { return std::fabs(v) == 1.0f; }));  // limited
}

TEST_CASE("a mixer without a bank plays nothing", "[presentation][audio]") {
  Mixer m;
  CHECK(m.play(SoundId::Hit) == -1);
  std::vector<float> out(64, 1.0f);
  m.mixInto(out.data(), 32);
  CHECK(std::ranges::all_of(out, [](float v) { return v == 0.0f; }));
}
