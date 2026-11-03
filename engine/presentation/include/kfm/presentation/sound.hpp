// Sound identifiers and the per-frame sound cues of a RenderState
// (architecture §7 "Audio"). The builder turns SimEvents into cues beside the
// visual effects (event_fx.hpp); the app hands each cue to a Mixer
// (mixer.hpp), which plays the clip a SoundBank (sound_bank.hpp) synthesised
// for that id. Nothing here touches SDL: the device lives in engine/platform.
#pragma once

#include <cstdint>

namespace kfm::presentation {

enum class SoundId : uint8_t {
  ThrowWhoosh = 0,
  DashSwish,
  CatchPerfect,  // dash-catch chime
  CatchAuto,     // owner catches a returning banana: a short tick
  Hit,           // banana or dash hit: thud
  Elimination,
  Pickup,
  BananaGround,  // a banana lands and becomes a pickup
  WallBounce,
  CountIn,  // every whole second of the round-start countdown
  RoundGo,  // the round starts (RoundStarting -> Playing)
  RoundEnd,
  MatchEnd,
  UiClick,  // played by the app (lobby, menus), never by an event
  // v0.23 (modes):
  Respawn,       // a deathmatch respawn: a rising shimmer
  Score,         // an elimination scored in deathmatch: a bright coin blip
  TimerWarning,  // the mode clock: 10, 3, 2, 1 seconds left
  // v0.20 (power-ups):
  PowerUp,      // a power-up taken: a rising arpeggio (v0.20)
  ShieldBreak,  // a shield absorbed a hit: a glassy crack (v0.20)
  // v0.21 (abilities):
  Blink,  // a blink: a rising zap
  Slam,   // a slam lands: a deep boom with a dusty crack
  Parry,  // a banana parried: a bright metallic clang
  Count,
};

inline constexpr uint8_t kSoundCount = static_cast<uint8_t>(SoundId::Count);

[[nodiscard]] constexpr const char* soundName(SoundId id) {
  switch (id) {
    case SoundId::ThrowWhoosh:
      return "throw";
    case SoundId::DashSwish:
      return "dash";
    case SoundId::CatchPerfect:
      return "catch-perfect";
    case SoundId::CatchAuto:
      return "catch-auto";
    case SoundId::Hit:
      return "hit";
    case SoundId::Elimination:
      return "elimination";
    case SoundId::Pickup:
      return "pickup";
    case SoundId::BananaGround:
      return "banana-ground";
    case SoundId::WallBounce:
      return "wall-bounce";
    case SoundId::CountIn:
      return "count-in";
    case SoundId::RoundGo:
      return "round-go";
    case SoundId::RoundEnd:
      return "round-end";
    case SoundId::MatchEnd:
      return "match-end";
    case SoundId::UiClick:
      return "ui-click";
    case SoundId::Respawn:
      return "respawn";
    case SoundId::Score:
      return "score";
    case SoundId::TimerWarning:
      return "timer-warning";
    case SoundId::PowerUp:
      return "power-up";
    case SoundId::ShieldBreak:
      return "shield-break";
    case SoundId::Blink:
      return "blink";
    case SoundId::Slam:
      return "slam";
    case SoundId::Parry:
      return "parry";
    case SoundId::Count:
      break;
  }
  return "?";
}

// One sound to start this frame. gain multiplies the clip's own gain; pan is
// -1 (left) .. +1 (right), 0 for unpositioned sounds.
struct SoundCue {
  SoundId id{SoundId::UiClick};
  float gain{1.0f};
  float pan{0.0f};
};

}  // namespace kfm::presentation
