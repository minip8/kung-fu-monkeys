// Procedural sound effects (architecture §7 "Audio"). Every clip is a small
// recipe of oscillators, filtered noise and envelopes, rendered once at
// startup and normalised to SoundBank::kClipPeak. The recipes are the audio
// "assets": change them here, listen in the client (F1 overlay, Audio).
#include "kfm/presentation/sound_bank.hpp"

#include "kfm/presentation/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace kfm::presentation {
namespace {

struct Recipe {
  float seconds;
  float gain;
};

// Indexed by SoundId.
constexpr std::array<Recipe, kSoundCount> kRecipes = {{
    {0.24f, 0.55f},   // ThrowWhoosh
    {0.16f, 0.45f},   // DashSwish
    {0.55f, 0.90f},   // CatchPerfect
    {0.07f, 0.50f},   // CatchAuto
    {0.28f, 0.90f},   // Hit
    {0.65f, 1.00f},   // Elimination
    {0.20f, 0.50f},   // Pickup
    {0.12f, 0.35f},   // BananaGround
    {0.08f, 0.30f},   // WallBounce
    {0.14f, 0.60f},   // CountIn
    {0.40f, 0.70f},   // RoundGo
    {0.70f, 0.75f},   // RoundEnd
    {1.50f, 0.85f},   // MatchEnd
    {0.035f, 0.50f},  // UiClick
    {0.45f, 0.55f},   // Respawn
    {0.22f, 0.60f},   // Score
    {0.12f, 0.65f},   // TimerWarning
    {0.36f, 0.60f},   // PowerUp
    {0.30f, 0.70f},   // ShieldBreak
    {0.18f, 0.50f},   // Blink
    {0.50f, 0.95f},   // Slam
    {0.32f, 0.75f},   // Parry
    {0.20f, 0.45f},   // CallPing
    {0.62f, 0.55f},   // Fall
    {0.20f, 0.45f},   // BananaPlop
    {0.30f, 0.45f},   // Regrow
}};

constexpr float kFadeInSeconds = 0.001f;
constexpr float kFadeOutSeconds = 0.004f;

// xorshift32 white noise in [-1, 1]; a fixed seed per clip keeps the bank
// identical on every run and platform.
class Noise {
 public:
  explicit Noise(uint32_t seed) : s_(seed != 0 ? seed : 0x9E3779B9u) {}
  float next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return (static_cast<float>(s_) * (2.0f / 4294967295.0f)) - 1.0f;
  }

 private:
  uint32_t s_;
};

// Chamberlin state-variable filter: low- and band-pass outputs.
class Svf {
 public:
  void step(float in, float cutoffHz, float sampleRate, float damping) {
    const float fc = std::min(cutoffHz, sampleRate * 0.2f);
    const float f = 2.0f * std::sin(kPi * fc / sampleRate);
    low_ += f * band_;
    const float high = in - low_ - (damping * band_);
    band_ += f * high;
  }
  [[nodiscard]] float low() const { return low_; }
  [[nodiscard]] float band() const { return band_; }

 private:
  float low_{0.0f};
  float band_{0.0f};
};

// Linear attack, then exponential decay with time constant `decay`.
[[nodiscard]] float env(float t, float attack, float decay) {
  if (t < 0.0f) return 0.0f;
  if (t < attack) return t / attack;
  return std::exp(-(t - attack) / decay);
}

// A sine oscillator with a running phase, so chirps stay continuous.
class Osc {
 public:
  float next(float hz, float sampleRate) {
    const float v = std::sin(phase_);
    phase_ = std::fmod(phase_ + (kTwoPi * hz / sampleRate), kTwoPi);
    return v;
  }
  [[nodiscard]] float phase() const { return phase_; }

 private:
  float phase_{0.0f};
};

// Sum of harmonics 1..3 of a phase, weights a1..a3.
[[nodiscard]] float harmonics(float phase, float a2, float a3) {
  return std::sin(phase) + (a2 * std::sin(2.0f * phase)) + (a3 * std::sin(3.0f * phase));
}

// A bell-ish note: inharmonic partials, the upper ones decaying faster.
[[nodiscard]] float bell(float t, float hz, float decay) {
  if (t < 0.0f) return 0.0f;
  const float a = env(t, 0.003f, decay);
  const float b = env(t, 0.003f, decay * 0.5f);
  const float c = env(t, 0.003f, decay * 0.25f);
  return (a * std::sin(kTwoPi * hz * t)) + (0.5f * b * std::sin(kTwoPi * hz * 2.76f * t)) +
         (0.25f * c * std::sin(kTwoPi * hz * 5.40f * t));
}

// A soft triangle-like tone (odd harmonics 1, 3, 5).
[[nodiscard]] float softTriangle(float t, float hz) {
  const float w = kTwoPi * hz * t;
  return std::sin(w) - (std::sin(3.0f * w) / 9.0f) + (std::sin(5.0f * w) / 25.0f);
}

void synthesise(SoundId id, std::span<float> out, float sr) {
  Noise noise(0xC0FFEEu + (static_cast<uint32_t>(id) * 7919u));
  Svf svf;
  Svf svf2;
  Osc osc;
  const float len = static_cast<float>(out.size()) / sr;
  for (std::size_t i = 0; i < out.size(); ++i) {
    const float t = static_cast<float>(i) / sr;
    const float u = len > 0.0f ? t / len : 0.0f;
    float v = 0.0f;
    switch (id) {
      case SoundId::ThrowWhoosh: {
        // Band-passed noise sweeping up and back down, swelling in the middle.
        const float centre = 500.0f + (2000.0f * std::sin(kPi * std::min(u * 1.3f, 1.0f)));
        svf.step(noise.next(), centre, sr, 0.6f);
        v = svf.band() * std::pow(std::sin(kPi * u), 1.5f);
        break;
      }
      case SoundId::DashSwish: {
        // A brighter, shorter swish rising in pitch, front-loaded.
        svf.step(noise.next(), 1500.0f + (3000.0f * u), sr, 0.5f);
        v = svf.band() * std::pow(std::sin(kPi * std::sqrt(u)), 2.0f);
        break;
      }
      case SoundId::CatchPerfect:
        // Two-note chime (E6 then B6).
        v = bell(t, 1318.5f, 0.18f) + (0.8f * bell(t - 0.07f, 1975.5f, 0.2f));
        break;
      case SoundId::CatchAuto:
        v = env(t, 0.001f, 0.015f) * (osc.next(2200.0f, sr) + (0.3f * noise.next()));
        break;
      case SoundId::Hit: {
        // A pitch-dropping body thud plus a short low-passed noise smack.
        const float hz = 50.0f + (110.0f * std::exp(-t / 0.04f));
        svf.step(noise.next(), 900.0f, sr, 0.8f);
        v = (env(t, 0.002f, 0.09f) * osc.next(hz, sr)) + (0.6f * env(t, 0.001f, 0.03f) * svf.low());
        break;
      }
      case SoundId::Elimination: {
        // A falling three-harmonic tone over a band-passed poof.
        const float hz = 90.0f + (520.0f * std::exp(-t * 2.6f));
        osc.next(hz, sr);
        svf.step(noise.next(), 400.0f, sr, 0.7f);
        v = (env(t, 0.01f, 0.25f) * harmonics(osc.phase(), 0.5f, 0.33f)) +
            (0.5f * env(t, 0.002f, 0.12f) * svf.band());
        break;
      }
      case SoundId::Pickup: {
        // Two rising blips (B5, F#6).
        const float hz = t < 0.08f ? 988.0f : 1480.0f;
        const float e = t < 0.08f ? env(t, 0.003f, 0.05f) : env(t - 0.08f, 0.003f, 0.05f);
        osc.next(hz, sr);
        v = e * harmonics(osc.phase(), 0.0f, 0.2f);
        break;
      }
      case SoundId::BananaGround: {
        const float hz = 80.0f + (100.0f * std::exp(-t / 0.03f));
        svf.step(noise.next(), 600.0f, sr, 0.8f);
        v = (env(t, 0.002f, 0.035f) * osc.next(hz, sr)) + (0.3f * env(t, 0.0f, 0.01f) * svf.low());
        break;
      }
      case SoundId::WallBounce: {
        osc.next(620.0f, sr);
        v = (env(t, 0.001f, 0.018f) * harmonics(osc.phase(), 0.3f, 0.0f)) +
            (0.5f * env(t, 0.0f, 0.004f) * noise.next());
        break;
      }
      case SoundId::CountIn:
      case SoundId::RoundGo: {
        const bool go = id == SoundId::RoundGo;
        const float hold = go ? 0.3f : 0.1f;
        osc.next(go ? 1760.0f : 880.0f, sr);
        // Quick attack, held, then a linear release to the end.
        const float release = clamp01(1.0f - ((t - hold) / std::max(len - hold, 1e-3f)));
        const float e = std::min({clamp01(t / 0.005f), 1.0f, t < hold ? 1.0f : release});
        v = e * harmonics(osc.phase(), 0.0f, 0.15f);
        break;
      }
      case SoundId::RoundEnd:
        // G5 then C6.
        v = (env(t, 0.005f, 0.25f) * softTriangle(t, 784.0f)) +
            (t >= 0.15f ? env(t - 0.15f, 0.005f, 0.25f) * softTriangle(t, 1046.5f) : 0.0f);
        break;
      case SoundId::MatchEnd: {
        // C major arpeggio ringing into a chord.
        constexpr std::array<float, 4> kNotes = {523.25f, 659.25f, 784.0f, 1046.5f};
        for (std::size_t n = 0; n < kNotes.size(); ++n) {
          const float start = 0.12f * static_cast<float>(n);
          if (t >= start) v += env(t - start, 0.005f, 0.5f) * softTriangle(t, kNotes[n]);
        }
        break;
      }
      case SoundId::UiClick:
        svf2.step(noise.next(), 3000.0f, sr, 0.7f);
        v = (env(t, 0.0005f, 0.006f) * osc.next(1500.0f, sr)) +
            (0.3f * env(t, 0.0f, 0.002f) * svf2.band());
        break;
      case SoundId::Respawn: {
        // A rising shimmer: a sine gliding up an octave with a soft tremolo,
        // swelling in and fading out.
        const float hz = 440.0f * std::pow(2.0f, u);
        osc.next(hz, sr);
        const float tremolo = 0.75f + (0.25f * std::sin(kTwoPi * 18.0f * t));
        v = std::sin(kPi * u) * tremolo * harmonics(osc.phase(), 0.25f, 0.1f);
        break;
      }
      case SoundId::Score:
        // A coin blip: B5 jumping to E6.
        osc.next(t < 0.06f ? 987.8f : 1318.5f, sr);
        v = (t < 0.06f ? env(t, 0.002f, 0.04f) : env(t - 0.06f, 0.002f, 0.09f)) *
            harmonics(osc.phase(), 0.0f, 0.3f);
        break;
      case SoundId::TimerWarning: {
        // A short square-ish beep (A5) held, then released.
        osc.next(880.0f, sr);
        const float release = clamp01(1.0f - ((t - 0.06f) / std::max(len - 0.06f, 1e-3f)));
        v = std::min(clamp01(t / 0.003f), t < 0.06f ? 1.0f : release) *
            harmonics(osc.phase(), 0.0f, 0.33f);
        break;
      }
      case SoundId::PowerUp: {
        // A rising major arpeggio (C6, E6, G6, C7), 80 ms per note.
        constexpr std::array<float, 4> kNotes = {1046.5f, 1318.5f, 1568.0f, 2093.0f};
        const auto note = std::min<std::size_t>(static_cast<std::size_t>(t / 0.08f), 3);
        const float start = static_cast<float>(note) * 0.08f;
        osc.next(kNotes[note], sr);
        v = env(t - start, 0.003f, note == 3 ? 0.12f : 0.06f) * harmonics(osc.phase(), 0.0f, 0.25f);
        break;
      }
      case SoundId::ShieldBreak: {
        // A glassy crack: bright band-passed noise over a falling ring.
        const float hz = 1800.0f + (1400.0f * std::exp(-t / 0.05f));
        svf.step(noise.next(), 4200.0f, sr, 0.4f);
        osc.next(hz, sr);
        v = (0.7f * env(t, 0.001f, 0.05f) * svf.band()) +
            (env(t, 0.002f, 0.12f) * harmonics(osc.phase(), 0.4f, 0.2f));
        break;
      }
      case SoundId::Blink: {
        // A rising zap: a sine chirping up two octaves with a band-passed hiss.
        const float hz = 600.0f * std::pow(4.0f, u);
        svf.step(noise.next(), 2500.0f + (4000.0f * u), sr, 0.5f);
        osc.next(hz, sr);
        v = std::sin(kPi * std::sqrt(u)) *
            ((0.7f * harmonics(osc.phase(), 0.3f, 0.0f)) + (0.4f * svf.band()));
        break;
      }
      case SoundId::Slam: {
        // A deep boom (pitch falling to 40 Hz) under a short low-passed crack.
        const float hz = 40.0f + (80.0f * std::exp(-t / 0.06f));
        svf.step(noise.next(), 700.0f, sr, 0.9f);
        osc.next(hz, sr);
        v = (env(t, 0.003f, 0.18f) * harmonics(osc.phase(), 0.4f, 0.15f)) +
            (0.7f * env(t, 0.001f, 0.05f) * svf.low());
        break;
      }
      case SoundId::Parry:
        // A metallic clang: two inharmonic bells over a click.
        v = bell(t, 1560.0f, 0.12f) + (0.6f * bell(t, 2410.0f, 0.08f)) +
            (0.5f * env(t, 0.0f, 0.003f) * noise.next());
        break;
      case SoundId::CallPing:
        // A call (v0.35): two quick bell notes a fifth apart.
        v = bell(t, 1760.0f, 0.05f) + (0.7f * bell(t - 0.06f, 2637.0f, 0.06f));
        break;
      case SoundId::Fall: {
        // v0.57: a slide whistle falling from 1.4 kHz to under 300 Hz with a
        // widening wobble, fading as the monkey drops out of earshot.
        const float hz = 260.0f + (1140.0f * std::exp(-t * 4.2f));
        const float wobble = 1.0f + (0.03f * u * std::sin(kTwoPi * 7.0f * t));
        osc.next(hz * wobble, sr);
        v = std::min(clamp01(t / 0.02f), 1.0f - (u * u)) * harmonics(osc.phase(), 0.2f, 0.05f);
        break;
      }
      case SoundId::BananaPlop: {
        // v0.57: a bloop falling from 900 Hz with a dab of low-passed splash.
        const float hz = 220.0f + (680.0f * std::exp(-t / 0.05f));
        svf.step(noise.next(), 1200.0f, sr, 0.7f);
        osc.next(hz, sr);
        v = (env(t, 0.004f, 0.06f) * harmonics(osc.phase(), 0.25f, 0.0f)) +
            (0.25f * env(t, 0.002f, 0.03f) * svf.low());
        break;
      }
      case SoundId::Regrow: {
        // v0.57: a soft pop gliding up from 520 Hz to an octave above, then a
        // small bell twinkle on top.
        const float hz = 520.0f * std::pow(2.0f, clamp01(t / 0.12f));
        osc.next(hz, sr);
        v = (env(t, 0.006f, 0.1f) * harmonics(osc.phase(), 0.15f, 0.0f)) +
            (0.5f * bell(t - 0.12f, 2093.0f, 0.07f));
        break;
      }
      case SoundId::Count:
        break;
    }
    out[i] = v;
  }
}

// Normalise the peak to `peak`, then fade the ends against clicks.
void finish(std::span<float> s, float peak, float sr) {
  float maxAbs = 0.0f;
  for (const float v : s)
    maxAbs = std::max(maxAbs, std::fabs(v));
  const float k = maxAbs > 1e-6f ? peak / maxAbs : 0.0f;
  const auto fadeIn = static_cast<std::size_t>(kFadeInSeconds * sr);
  const auto fadeOut = static_cast<std::size_t>(kFadeOutSeconds * sr);
  for (std::size_t i = 0; i < s.size(); ++i) {
    float g = k;
    if (i < fadeIn) g *= static_cast<float>(i) / static_cast<float>(fadeIn);
    const std::size_t fromEnd = s.size() - 1 - i;
    if (fromEnd < fadeOut) g *= static_cast<float>(fromEnd) / static_cast<float>(fadeOut);
    s[i] *= g;
  }
}

}  // namespace

float soundLengthSeconds(SoundId id) {
  return kRecipes[static_cast<uint8_t>(id) % kSoundCount].seconds;
}

void SoundBank::generate(int sampleRate) {
  sampleRate_ = std::clamp(sampleRate, 8000, 192000);
  const auto sr = static_cast<float>(sampleRate_);
  for (uint8_t i = 0; i < kSoundCount; ++i) {
    const auto id = static_cast<SoundId>(i);
    SoundClip& c = clips_[i];
    c.gain = kRecipes[i].gain;
    c.samples.assign(static_cast<std::size_t>(std::lround(kRecipes[i].seconds * sr)), 0.0f);
    synthesise(id, c.samples, sr);
    finish(c.samples, kClipPeak, sr);
  }
}

}  // namespace kfm::presentation
