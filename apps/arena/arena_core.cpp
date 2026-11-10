// kfm_arena's core (see arena_core.hpp).
#include "arena_core.hpp"

#include "kfm/ai/agent_host.hpp"
#include "kfm/core/bitstream.hpp"
#include "kfm/sim/constants.hpp"
#include "kfm/sim/events.hpp"
#include "kfm/sim/input.hpp"
#include "kfm/sim/replay.hpp"
#include "kfm/sim/step.hpp"

#ifdef KFM_WASM_BOTS
#include "kfm/ai/wasm_runtime.hpp"
#endif

#include <algorithm>
#include <format>
#include <iterator>
#include <numeric>
#include <string_view>
#include <utility>

namespace kfm::arena {

// ---------------------------------------------------------------------------
// runMatch
// ---------------------------------------------------------------------------

void assignPlaces(MatchResult& result) {
  std::vector<size_t> order(result.slots.size());
  std::ranges::iota(order, size_t{0});
  const auto isWinner = [&](const SlotResult& s) {
    return result.winner.isValid() && s.slot == result.winner.value;
  };
  std::ranges::sort(order, [&](size_t ia, size_t ib) {
    const SlotResult& a = result.slots[ia];
    const SlotResult& b = result.slots[ib];
    if (isWinner(a) != isWinner(b)) return isWinner(a);
    if (a.score != b.score) return a.score > b.score;
    if (a.deaths != b.deaths) return a.deaths < b.deaths;
    return a.slot < b.slot;
  });
  for (size_t place = 0; place < order.size(); ++place) {
    result.slots[order[place]].place = static_cast<uint8_t>(place + 1);
  }
}

void assignTeamPlaces(MatchResult& result) {
  std::vector<size_t> order(result.teams.size());
  std::ranges::iota(order, size_t{0});
  int winnerTeam = -1;
  if (result.winner.isValid() && result.winner.value < result.slots.size()) {
    winnerTeam = result.slots[result.winner.value].team;
  }
  std::ranges::sort(order, [&](size_t ia, size_t ib) {
    const TeamResult& a = result.teams[ia];
    const TeamResult& b = result.teams[ib];
    const bool wa = int{a.team} == winnerTeam;
    const bool wb = int{b.team} == winnerTeam;
    if (wa != wb) return wa;
    if (a.score != b.score) return a.score > b.score;
    if (a.deaths != b.deaths) return a.deaths < b.deaths;
    return a.team < b.team;
  });
  for (size_t place = 0; place < order.size(); ++place) {
    result.teams[order[place]].place = static_cast<uint8_t>(place + 1);
  }
}

std::expected<MatchResult, BotError> runMatch(const MatchSetup& setup,
                                              const sim::ContentDB& content, BotFactory& factory,
                                              uint32_t index) {
  using namespace kfm::sim;
  MatchConfig config = setup.config;
  // The spec and the bot index of every slot.
  std::vector<const ai::BotSpec*> specOf;
  std::vector<uint8_t> botOf;
  std::vector<uint8_t> entrantOf;  // by team index (team matches)
  const bool teamMatch = !setup.teams.empty();
  if (teamMatch) {
    const ModeDef& mode = content.modeAt(config.mode);
    const size_t teams = setup.teams.size();
    if (mode.teams < 2 || teams != mode.teams) {
      return std::unexpected(
          BotError{BotErrorKind::Load, std::format("mode {} has {} teams, got {} --team entrants",
                                                   mode.name.view(), unsigned{mode.teams}, teams)});
    }
    size_t size = 1;
    for (const std::vector<ai::BotSpec>& t : setup.teams) {
      if (t.empty()) return std::unexpected(BotError{BotErrorKind::Load, "an empty --team"});
      size = std::max(size, t.size());
    }
    if (teams * size > kMaxPlayers) {
      return std::unexpected(
          BotError{BotErrorKind::Load, std::format("{} teams of {} make {} slots (at most {})",
                                                   teams, size, teams * size, kMaxPlayers)});
    }
    const size_t n = teams * size;
    config.players = static_cast<PlayerMask>((1u << n) - 1u);
    config.teams = teamsFor(mode, config.players);
    entrantOf.resize(teams);
    for (size_t t = 0; t < teams; ++t)
      entrantOf[t] = static_cast<uint8_t>((t + teams - (setup.rotation % teams)) % teams);
    std::vector<size_t> position(teams, 0);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t t = config.teams[i];
      const uint8_t e = entrantOf[t];
      const std::vector<ai::BotSpec>& list = setup.teams[e];
      const size_t p = position[t]++;
      specOf.push_back(&list[std::min(p, list.size() - 1)]);
      botOf.push_back(static_cast<uint8_t>((e * size) + p));
    }
  } else {
    const size_t n = setup.bots.size();
    if (n == 0 || n > kMaxPlayers) {
      return std::unexpected(BotError{
          BotErrorKind::Load, std::format("a match needs 1..{} bots, got {}", kMaxPlayers, n)});
    }
    config.players = static_cast<PlayerMask>((1u << n) - 1u);
    // Slot i plays bot botOf[i]: bot b sits in slot (b + rotation) mod n.
    const size_t shift = setup.rotation % n;
    for (size_t i = 0; i < n; ++i) {
      const size_t b = (i + n - shift) % n;
      specOf.push_back(&setup.bots[b]);
      botOf.push_back(static_cast<uint8_t>(b));
    }
  }
  const size_t n = specOf.size();
  const MatchContent match = matchContent(content, config);

  // Agents first: a failing module leaves no half-played match.
  ai::AgentHost host;
  host.setRespawnMemory(setup.memory);
  std::vector<ai::WasmAgent*> wasm(n, nullptr);
  for (size_t i = 0; i < n; ++i) {
    auto made = factory.make(*specOf[i]);
    if (!made) return std::unexpected(std::move(made.error()));
    const auto slot = static_cast<uint8_t>(i);
    wasm[i] = made->wasm;
    host.attach(PlayerId{slot}, std::move(made->agent), ai::agentSeed(config.seed, PlayerId{slot}),
                setup.delayTicks);
  }

  MatchResult result;
  result.index = index;
  result.seed = config.seed;
  result.contentHash = contentHash(content);
  result.buildHash = 0;  // no build stamps a hash yet (kfm_replay writes 0 too)
  result.slots.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const ai::BotSpec& bot = *specOf[i];
    result.slots[i].slot = static_cast<uint8_t>(i);
    result.slots[i].botIndex = botOf[i];
    result.slots[i].team = config.teams[i];
    result.slots[i].bot = ai::toString(bot);
    result.slots[i].module = bot.kind == ai::BotKind::Module;
  }
  std::vector<uint32_t> teamKills(teamMatch ? setup.teams.size() : 0, 0);

  // A WorldState is large: keep it off the stack.
  const auto world = std::make_unique<WorldState>(initialWorld(config));
  EventSink sink;
  std::vector<InputFrame> frames;
  if (setup.recordReplay) frames.reserve(std::min<uint32_t>(setup.tickCap, 1u << 16));

  for (uint32_t t = 0; t < setup.tickCap; ++t) {
    InputFrame frame;
    frame.tick = world->tick;
    frame.present = world->rules.present;
    // fill, step, observe (v0.35: the calls each bot hears) in one call.
    host.step(*world, frame, match, sink);
    if (setup.recordReplay) frames.push_back(frame);
    ++result.ticks;
    bool ended = false;
    for (const SimEvent& e : sink) {
      if (e.kind == EventKind::PlayerEliminated) {
        if (e.player.isValid() && e.player.value < n) ++result.slots[e.player.value].deaths;
        if (e.a < n && e.a != e.player.value) ++result.slots[e.a].kills;
        if (teamMatch && e.a < n && e.player.isValid() && e.player.value < n &&
            config.teams[e.a] != config.teams[e.player.value] &&
            config.teams[e.a] < teamKills.size()) {
          ++teamKills[config.teams[e.a]];
        }
      } else if (e.kind == EventKind::MatchEnded) {
        ended = true;
        result.outcome = MatchOutcome::Ended;
        result.winner = e.player.isValid() && e.player.value < n ? e.player : PlayerId::invalid();
      }
    }
    if (ended) break;
  }

  for (size_t i = 0; i < n; ++i) {
    SlotResult& s = result.slots[i];
    // An int8_t score is a number, not a character (v0.57: signed scores).
    s.score = world->rules.scores[i];  // NOLINT(bugprone-signed-char-misuse)
    const ai::AgentSlotStats stats = host.stats(PlayerId{static_cast<uint8_t>(i)});
    s.acts = stats.acts;
    s.substitutions = stats.substitutions;
    s.paused = stats.paused;
#ifdef KFM_WASM_BOTS
    if (wasm[i] != nullptr) {
      const ai::WasmAgentStats& w = wasm[i]->stats();
      s.traps = w.traps;
      s.overBudget = w.overBudget;
      s.badReturns = w.badReturns;
      s.fuelMean = w.acts == 0 ? 0 : w.fuelTotal / w.acts;
      s.fuelMax = w.fuelMax;
      s.fuelInit = w.fuelInit;
      s.logBytes = w.logBytes;
      if (!wasm[i]->alive()) s.dead = std::string(ai::toString(wasm[i]->deathCause()));
      s.log = wasm[i]->takeLog();
    }
#endif
  }
  result.checksum = checksum(*world);
  assignPlaces(result);
  if (teamMatch) {
    result.teams.resize(setup.teams.size());
    for (size_t t = 0; t < result.teams.size(); ++t) {
      TeamResult& tr = result.teams[t];
      tr.team = static_cast<uint8_t>(t);
      tr.entrant = entrantOf[t];
      tr.score = match.teamScore(world->rules.scores, tr.team);
      tr.kills = teamKills[t];
      for (const SlotResult& s : result.slots) {
        if (s.team != tr.team) continue;
        if (!tr.bots.empty()) tr.bots.push_back(',');
        tr.bots += s.bot;
        tr.deaths += s.deaths;
      }
    }
    assignTeamPlaces(result);
  }

  if (setup.recordReplay) {
    ReplayHeader h;
    h.seed = config.seed;
    h.contentHash = result.contentHash;
    h.buildHash = result.buildHash;
    h.players = config.players;
    h.arena = config.arena;
    h.mode = config.mode;
    h.powerUps = config.powerUps;
    h.loadout = config.loadout;
    h.teams = config.teams;
    h.tickCount = result.ticks;
    result.replay.resize(replayMaxBytes(h));
    BitWriter w(result.replay);
    writeReplayHeader(w, h);
    for (const InputFrame& f : frames)
      writeReplayFrame(w, f);
    result.replay.resize(w.ok() ? w.bytesWritten() : 0);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Summary
// ---------------------------------------------------------------------------

void Summary::add(const MatchResult& result) {
  ++matches;
  ticks += result.ticks;
  if (result.outcome == MatchOutcome::Cap) {
    ++capped;
  } else if (result.winner.isValid()) {
    ++ended;
  } else {
    ++draws;
  }
  const bool won = result.winner.isValid();
  const auto fold = [&](std::vector<SlotSummary>& rows, uint8_t index, const SlotResult& s) {
    if (rows.size() <= index) rows.resize(size_t{index} + 1);
    SlotSummary& t = rows[index];
    const bool first = t.bot.empty();
    t.index = index;
    if (first) {
      t.bot = s.bot;
    } else if (t.bot != s.bot) {
      t.bot = "mixed";
    }
    t.module = t.module || s.module;
    if (won && result.winner.value == s.slot) ++t.wins;
    t.placeSum += s.place;
    t.score += s.score;
    t.kills += s.kills;
    t.deaths += s.deaths;
    t.acts += s.acts;
    t.substitutions += s.substitutions;
    t.traps += s.traps;
    t.overBudget += s.overBudget;
    t.badReturns += s.badReturns;
    t.fuelMax = std::max(t.fuelMax, s.fuelMax);
  };
  for (const SlotResult& s : result.slots) {
    fold(slots, s.slot, s);
    fold(bots, s.botIndex, s);
    if (s.botIndex != s.slot) rotated = true;
  }
  // v0.35: per entrant of a team match.
  int winnerTeam = -1;
  if (won && result.winner.value < result.slots.size()) {
    winnerTeam = result.slots[result.winner.value].team;
  }
  for (const TeamResult& t : result.teams) {
    if (entrants.size() <= t.entrant) entrants.resize(size_t{t.entrant} + 1);
    EntrantSummary& e = entrants[t.entrant];
    e.index = t.entrant;
    e.bots = t.bots;
    if (int{t.team} == winnerTeam) ++e.wins;
    e.placeSum += t.place;
    e.score += t.score;
    e.kills += t.kills;
    e.deaths += t.deaths;
  }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

namespace {

std::string_view outcomeText(MatchOutcome o) {
  return o == MatchOutcome::Ended ? "ended" : "cap";
}

std::string winnerText(PlayerId w) {
  return w.isValid() ? std::to_string(w.value) : std::string("none");
}

// The winning team of a team match (the winner slot's), or none.
std::string winnerTeamText(const MatchResult& r) {
  if (!r.winner.isValid() || r.winner.value >= r.slots.size()) return "none";
  return std::to_string(r.slots[r.winner.value].team);
}

// A JSON string literal: quotes, backslashes and control characters escaped.
std::string jsonString(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          std::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
  return out;
}

double meanPlace(const SlotSummary& s, uint32_t matches) {
  return matches == 0 ? 0.0 : static_cast<double>(s.placeSum) / static_cast<double>(matches);
}

double meanEntrantPlace(const EntrantSummary& e, uint32_t matches) {
  return matches == 0 ? 0.0 : static_cast<double>(e.placeSum) / static_cast<double>(matches);
}

}  // namespace

std::string formatResultLine(const MatchResult& r) {
  std::string out =
      std::format("match {} seed {} ticks {} checksum {:08x} result {} winner {}", r.index, r.seed,
                  r.ticks, r.checksum, outcomeText(r.outcome), winnerText(r.winner));
  const bool teams = !r.teams.empty();
  if (teams) out += " winner_team " + winnerTeamText(r);
  for (const TeamResult& t : r.teams) {
    std::format_to(std::back_inserter(out),
                   " | team {} entrant {} {} place {} score {} kills {} deaths {}", t.team,
                   t.entrant, t.bots, t.place, t.score, t.kills, t.deaths);
  }
  for (const SlotResult& s : r.slots) {
    std::format_to(std::back_inserter(out), " | slot {} bot {} {}", s.slot, s.botIndex, s.bot);
    if (teams) std::format_to(std::back_inserter(out), " team {}", s.team);
    std::format_to(std::back_inserter(out), " place {} score {} kills {} deaths {} acts {} subs {}",
                   s.place, s.score, s.kills, s.deaths, s.acts, s.substitutions);
    if (s.module) {
      std::format_to(
          std::back_inserter(out),
          " traps {} over {} bad {} fuel_mean {} fuel_max {} fuel_init {} log {} dead {}", s.traps,
          s.overBudget, s.badReturns, s.fuelMean, s.fuelMax, s.fuelInit, s.logBytes,
          s.dead.empty() ? std::string_view("no") : std::string_view(s.dead));
    }
  }
  return out;
}

std::string formatResultJson(const MatchResult& r) {
  std::string out = std::format(
      R"({{"match":{},"seed":{},"ticks":{},"checksum":"{:08x}","result":"{}","winner":{})", r.index,
      r.seed, r.ticks, r.checksum, outcomeText(r.outcome),
      r.winner.isValid() ? std::to_string(r.winner.value) : "null");
  const bool teams = !r.teams.empty();
  if (teams) {
    const std::string w = winnerTeamText(r);
    std::format_to(std::back_inserter(out), R"(,"winner_team":{},"teams":[)",
                   w == "none" ? std::string("null") : w);
    for (size_t i = 0; i < r.teams.size(); ++i) {
      const TeamResult& t = r.teams[i];
      if (i != 0) out.push_back(',');
      std::format_to(std::back_inserter(out),
                     R"({{"team":{},"entrant":{},"spec":{},"place":{},"score":{},"kills":{},)"
                     R"("deaths":{}}})",
                     t.team, t.entrant, jsonString(t.bots), t.place, t.score, t.kills, t.deaths);
    }
    out.push_back(']');
  }
  out += R"(,"slots":[)";
  for (size_t i = 0; i < r.slots.size(); ++i) {
    const SlotResult& s = r.slots[i];
    if (i != 0) out.push_back(',');
    std::format_to(std::back_inserter(out), R"({{"slot":{},"bot":{},"spec":{},"module":{},)",
                   s.slot, s.botIndex, jsonString(s.bot), s.module);
    if (teams) std::format_to(std::back_inserter(out), R"("team":{},)", s.team);
    std::format_to(std::back_inserter(out),
                   R"("place":{},"score":{},"kills":{},"deaths":{},"acts":{},"subs":{})", s.place,
                   s.score, s.kills, s.deaths, s.acts, s.substitutions);
    if (s.module) {
      std::format_to(std::back_inserter(out),
                     R"(,"traps":{},"over":{},"bad":{},"fuel_mean":{},"fuel_max":{},)"
                     R"("fuel_init":{},"log":{},"dead":{})",
                     s.traps, s.overBudget, s.badReturns, s.fuelMean, s.fuelMax, s.fuelInit,
                     s.logBytes, s.dead.empty() ? std::string("null") : jsonString(s.dead));
    }
    out.push_back('}');
  }
  // v0.60, appended: the replay header's hashes, so a record names its content and build.
  std::format_to(std::back_inserter(out), R"(],"content_hash":"{:08x}","build_hash":"{:08x}"}})",
                 r.contentHash, r.buildHash);
  return out;
}

std::string formatSummaryLine(const Summary& sum) {
  const uint64_t ticksMean = sum.matches == 0 ? 0 : sum.ticks / sum.matches;
  std::string out = std::format("summary matches {} ended {} draws {} cap {} ticks_mean {}",
                                sum.matches, sum.ended, sum.draws, sum.capped, ticksMean);
  const auto rows = [&](std::string_view label, const std::vector<SlotSummary>& totals) {
    for (const SlotSummary& s : totals) {
      std::format_to(std::back_inserter(out),
                     " | {} {} {} wins {} mean_place {:.2f} score {} kills {} deaths {} acts {} "
                     "subs {}",
                     label, s.index, s.bot, s.wins, meanPlace(s, sum.matches), s.score, s.kills,
                     s.deaths, s.acts, s.substitutions);
      if (s.module) {
        std::format_to(std::back_inserter(out), " traps {} over {} bad {} fuel_max {}", s.traps,
                       s.overBudget, s.badReturns, s.fuelMax);
      }
    }
  };
  rows("slot", sum.slots);
  if (sum.rotated) rows("bot", sum.bots);
  for (const EntrantSummary& e : sum.entrants) {
    std::format_to(std::back_inserter(out),
                   " | entrant {} {} wins {} mean_place {:.2f} score {} kills {} deaths {}",
                   e.index, e.bots, e.wins, meanEntrantPlace(e, sum.matches), e.score, e.kills,
                   e.deaths);
  }
  return out;
}

std::string formatSummaryJson(const Summary& sum) {
  const uint64_t ticksMean = sum.matches == 0 ? 0 : sum.ticks / sum.matches;
  std::string out =
      std::format(R"({{"summary":true,"matches":{},"ended":{},"draws":{},"cap":{},"ticks_mean":{})",
                  sum.matches, sum.ended, sum.draws, sum.capped, ticksMean);
  const auto rows = [&](std::string_view key, std::string_view label,
                        const std::vector<SlotSummary>& totals) {
    std::format_to(std::back_inserter(out), R"(,"{}":[)", key);
    for (size_t i = 0; i < totals.size(); ++i) {
      const SlotSummary& s = totals[i];
      if (i != 0) out.push_back(',');
      std::format_to(std::back_inserter(out),
                     R"({{"{}":{},"spec":{},"module":{},"wins":{},"mean_place":{:.2f},"score":{},)"
                     R"("kills":{},"deaths":{},"acts":{},"subs":{})",
                     label, s.index, jsonString(s.bot), s.module, s.wins, meanPlace(s, sum.matches),
                     s.score, s.kills, s.deaths, s.acts, s.substitutions);
      if (s.module) {
        std::format_to(std::back_inserter(out), R"(,"traps":{},"over":{},"bad":{},"fuel_max":{})",
                       s.traps, s.overBudget, s.badReturns, s.fuelMax);
      }
      out.push_back('}');
    }
    out.push_back(']');
  };
  rows("slots", "slot", sum.slots);
  if (sum.rotated) rows("bots", "bot", sum.bots);
  if (!sum.entrants.empty()) {
    out += R"(,"entrants":[)";
    for (size_t i = 0; i < sum.entrants.size(); ++i) {
      const EntrantSummary& e = sum.entrants[i];
      if (i != 0) out.push_back(',');
      std::format_to(std::back_inserter(out),
                     R"({{"entrant":{},"spec":{},"wins":{},"mean_place":{:.2f},"score":{},)"
                     R"("kills":{},"deaths":{}}})",
                     e.index, jsonString(e.bots), e.wins, meanEntrantPlace(e, sum.matches), e.score,
                     e.kills, e.deaths);
    }
    out.push_back(']');
  }
  out.push_back('}');
  return out;
}

}  // namespace kfm::arena
