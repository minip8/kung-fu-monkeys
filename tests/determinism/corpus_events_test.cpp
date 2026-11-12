// The determinism gate proves each corpus replay reproduces its checksums;
// it cannot tell whether a replay still exercises the feature it is in the
// corpus for. The corpus is scripted input, so a seed or content change can
// leave, say, the capture entry without a single capture and it would pass
// the gate forever. This test plays every entry of
// tools/replay-corpus/corpus.txt against the embedded content and asserts
// the events its name promises (architecture §9), and that no tick overflowed
// the event or contact lists. Every corpus line needs a row here: a new
// entry states what it is for.
#include "kfm/content/app_content.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace kfm;
using namespace kfm::sim;

namespace {

// What a whole replay emitted (and how it ended).
struct Tally {
  static constexpr size_t kKinds = 64;
  std::array<uint32_t, kKinds> kinds{};
  uint32_t powerUpsTaken{0};
  uint32_t flagsTaken{0};
  uint32_t objectivePoints{0};  // ScoreChanged with ScoreReason::Objective
  uint32_t roundsWon{0};        // RoundEnded with a winner
  uint32_t perfectCatches{0};
  uint32_t powerUpsSpawned{0};  // bit i: power-up i was laid by a spawner
  uint32_t treeFruit{0};        // v0.81: banana pickups laid on a tree
  uint32_t eventsDropped{0};    // EventSink overflow, summed over the ticks
  uint32_t contactsDropped{0};  // StepStats: contact-list overflow, summed
  MatchPhase finalPhase{MatchPhase::Lobby};

  [[nodiscard]] uint32_t of(EventKind k) const { return kinds[static_cast<size_t>(k)]; }
};
static_assert(static_cast<size_t>(EventKind::ZoneCaptured) < Tally::kKinds);

std::string describe(const Tally& t) {
  std::ostringstream o;
  o << "events by kind:";
  for (size_t k = 0; k < Tally::kKinds; ++k) {
    if (t.kinds[k] != 0) o << ' ' << k << '=' << t.kinds[k];
  }
  o << "; power-ups taken " << t.powerUpsTaken << ", flags taken " << t.flagsTaken
    << ", objective points " << t.objectivePoints << ", rounds won " << t.roundsWon
    << ", perfect catches " << t.perfectCatches << ", power-ups spawned 0x" << std::hex
    << t.powerUpsSpawned << std::dec << ", final phase " << static_cast<int>(t.finalPhase);
  return o.str();
}

const content::LoadedContent& embedded() {
  static const content::LoadedContent c = [] {
    content::LoadedContent out;
    content::Diagnostics diag;
    // No --content and no executable directory: the copy compiled in, the
    // content kfm_replay records the corpus with.
    REQUIRE(content::loadAppContent("", {}, out, diag));
    return out;
  }();
  return c;
}

std::filesystem::path corpusDir() {
  return std::filesystem::path{KFM_REPLAY_CORPUS_DIR};
}

std::vector<std::string> corpusNames() {
  std::ifstream f(corpusDir() / "corpus.txt");
  REQUIRE(f.good());
  std::vector<std::string> names;
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream words(line);
    std::string name;
    if (!(words >> name) || name.starts_with('#')) continue;
    names.push_back(name);
  }
  return names;
}

Tally play(const std::string& name) {
  std::ifstream f(corpusDir() / (name + ".kfmr"), std::ios::binary);
  REQUIRE(f.good());
  const std::vector<char> raw{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(raw.size());
  for (size_t i = 0; i < raw.size(); ++i)
    bytes[i] = static_cast<std::byte>(raw[i]);

  BitReader r(bytes);
  ReplayHeader header;
  REQUIRE(readReplayHeader(r, header));
  const content::LoadedContent& c = embedded();
  REQUIRE(header.contentHash == c.hash);
  REQUIRE(header.arena < c.db.arenaCount);
  REQUIRE(header.mode < c.db.modeCount);
  const MatchContent content = matchContent(c.db, header.matchConfig());
  WorldState world = initialWorld(header.matchConfig());
  EventSink sink;
  Tally t;
  for (uint32_t i = 0; i < header.tickCount; ++i) {
    InputFrame frame;
    REQUIRE(readReplayFrame(r, frame));
    sink.clear();
    StepStats stats;
    step(world, frame, content, sink, stats);
    t.contactsDropped += stats.contactsDropped;
    t.eventsDropped += sink.dropped;
    for (const SimEvent& e : sink) {
      const auto k = static_cast<size_t>(e.kind);
      if (k < Tally::kKinds) ++t.kinds[k];
      switch (e.kind) {
        case EventKind::PickupTaken:
          if (e.a == static_cast<uint32_t>(PickupKind::PowerUp)) ++t.powerUpsTaken;
          if (e.a == static_cast<uint32_t>(PickupKind::Flag)) ++t.flagsTaken;
          break;
        case EventKind::PickupSpawned:
          if (e.a == static_cast<uint32_t>(PickupKind::PowerUp) && e.b < 32) {
            t.powerUpsSpawned |= 1u << e.b;
          }
          // v0.81: a banana laid on a tree's position (a tree's fruit).
          if (e.a == static_cast<uint32_t>(PickupKind::Banana) && world.pickups.isAlive(e.entity)) {
            const Vec2 at = world.pickups.items[e.entity.index].pos;
            for (uint8_t n = 0; n < content.arena.pickupSpawnerCount; ++n) {
              const PickupSpawner& sp = content.arena.pickupSpawners[n];
              if (sp.isTree() && sp.pos == at) ++t.treeFruit;
            }
          }
          break;
        case EventKind::ScoreChanged:
          if (e.b == static_cast<uint32_t>(ScoreReason::Objective)) ++t.objectivePoints;
          break;
        case EventKind::RoundEnded:
          if (e.player.isValid()) ++t.roundsWon;
          break;
        case EventKind::BoomerangCaught:
          if (e.a == static_cast<uint32_t>(CatchHow::Perfect)) ++t.perfectCatches;
          break;
        default:
          break;
      }
    }
  }
  t.finalPhase = world.rules.phase;
  return t;
}

// One feature a corpus entry promises.
struct Expectation {
  std::string_view what;
  bool (*holds)(const Tally&, const ContentDB&);
};
struct Entry {
  std::string_view name;
  std::vector<Expectation> expect;
};

bool idle(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::BoomerangThrown) == 0 && t.of(EventKind::PlayerDashed) == 0;
}
bool started(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::RoundStarted) > 0;
}
bool fights(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::BoomerangThrown) > 0 && t.of(EventKind::PlayerDashed) > 0 &&
         t.of(EventKind::PlayerHit) > 0 && t.of(EventKind::PlayerEliminated) > 0;
}
bool roundWon(const Tally& t, const ContentDB& /*db*/) {
  return t.roundsWon > 0;
}
bool catches(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::BoomerangCaught) > 0;
}
bool bounces(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::BoomerangBounced) > 0;
}
bool falls(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::PlayerFell) > 0;
}
bool powerUpTaken(const Tally& t, const ContentDB& /*db*/) {
  return t.powerUpsTaken > 0;
}
// The powerset entry restricts the spawners to shield and big_banana.
bool onlyTheSet(const Tally& t, const ContentDB& db) {
  uint32_t allowed = 0;
  for (const std::string_view n : {"shield", "big_banana"}) {
    const auto i = db.findPowerUp(n);
    if (!i) return false;
    allowed |= 1u << *i;
  }
  return t.powerUpsSpawned != 0 && (t.powerUpsSpawned & ~allowed) == 0;
}
bool objectivePoint(const Tally& t, const ContentDB& /*db*/) {
  return t.objectivePoints > 0;
}
bool respawns(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::PlayerRespawned) > 0;
}
bool blinks(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::PlayerBlinked) > 0;
}
bool slams(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::SlamLanded) > 0;
}
bool parries(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::PlayerParryStarted) > 0;
}
bool hillHeld(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::HillHolderChanged) > 0;
}
bool zoneCaptured(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::ZoneCaptured) > 0;
}
bool flagTaken(const Tally& t, const ContentDB& /*db*/) {
  return t.flagsTaken > 0;
}
bool calls(const Tally& t, const ContentDB& /*db*/) {
  return t.of(EventKind::MonkeyCalled) > 0;
}
// v0.81, harvest: a banana banked is a point with ScoreReason::Objective.
bool banked(const Tally& t, const ContentDB& /*db*/) {
  return t.objectivePoints > 0;
}
bool fruit(const Tally& t, const ContentDB& /*db*/) {
  return t.treeFruit > 0;
}

const std::vector<Entry>& expectations() {
  static const std::vector<Entry> table = {
      {"empty_2p_60t", {{"no input, no action", idle}}},
      {"empty_10p_600t", {{"no input, no action", idle}, {"a round starts", started}}},
      {"scripted_4p_1200t", {{"throws, dashes, hits, eliminations", fights}}},
      {"scripted_10p_3600t",
       {{"throws, dashes, hits, eliminations", fights},
        {"a round won", roundWon},
        {"a catch", catches}}},
      {"scripted_4p_pillars_1800t", {{"a banana bounces off a wall", bounces}}},
      {"scripted_4p_corridors_1800t", {{"a banana bounces off a wall", bounces}}},
      {"scripted_4p_island_1800t", {{"a monkey falls into a pit", falls}}},
      {"scripted_4p_powerups_1800t", {{"a power-up taken", powerUpTaken}}},
      {"scripted_4p_powerset_1800t",
       {{"a power-up taken", powerUpTaken},
        {"spawners lay only shield and big_banana", onlyTheSet}}},
      {"scripted_4p_deathmatch_3600t",
       {{"an elimination scores", objectivePoint}, {"a respawn", respawns}}},
      {"scripted_4p_survival_1800t", {{"a round won", roundWon}}},
      {"scripted_4p_blink_1800t", {{"a blink", blinks}}},
      {"scripted_4p_slam_1800t", {{"a slam lands", slams}}},
      {"scripted_4p_parry_1800t", {{"a parry", parries}}},
      {"scripted_4p_team_rounds_1800t", {{"a team round won", roundWon}}},
      {"scripted_4p_team_deathmatch_3600t",
       {{"an elimination scores", objectivePoint}, {"a respawn", respawns}}},
      {"scripted_4p_hill_3600t", {{"the hill held", hillHeld}, {"a hill point", objectivePoint}}},
      {"scripted_4p_capture_3600t", {{"a flag taken", flagTaken}, {"a capture", objectivePoint}}},
      {"scripted_4p_grid_rounds_3600t",
       {{"throws, dashes, hits, eliminations", fights}, {"a round won", roundWon}}},
      {"scripted_4p_grid_rounds_1t_1800t",
       {{"throws, dashes, hits, eliminations", fights}, {"a round won", roundWon}}},
      {"scripted_10p_grid_rounds_1t_1800t",
       {{"throws, dashes, hits, eliminations", fights},
        {"a round won", roundWon},
        {"a call", calls}}},
      // v0.81: harvest on grove. The scripted steering walks straight at its
      // goal, so on grove it banks the opening hands and then stands at the
      // pillars between the homes and the rich trees: no fruit is gathered
      // and nothing is stolen.
      {"scripted_10p_contest_1800t",
       {{"a tree's fruit", fruit}, {"a banana banked", banked}, {"a call", calls}}},
      {"scripted_10p_control_1t_1800t",
       {{"a zone captured", zoneCaptured}, {"a zone point", objectivePoint}, {"a call", calls}}},
  };
  return table;
}

}  // namespace

TEST_CASE("every corpus entry exercises what its name promises", "[determinism][corpus]") {
  const std::vector<std::string> names = corpusNames();
  REQUIRE_FALSE(names.empty());
  for (const std::string& name : names) {
    INFO("corpus entry " << name);
    const Entry* entry = nullptr;
    for (const Entry& e : expectations()) {
      if (e.name == name) entry = &e;
    }
    REQUIRE(entry != nullptr);  // add a row to expectations() for a new corpus entry
    const Tally t = play(name);
    INFO(describe(t));
    CHECK(t.eventsDropped == 0);
    CHECK(t.contactsDropped == 0);
    for (const Expectation& x : entry->expect) {
      INFO("expects " << x.what);
      CHECK(x.holds(t, embedded().db));
    }
  }
  // No stale rows: every row names a corpus entry.
  for (const Entry& e : expectations()) {
    INFO("expectation row " << e.name);
    bool listed = false;
    for (const std::string& n : names)
      listed = listed || n == e.name;
    CHECK(listed);
  }
}
