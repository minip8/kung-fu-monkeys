// TOML content loader (architecture §5.10). Each section parser reads its
// keys through a Section, which converts and range-checks values, records
// diagnostics with file:line, and reports every key nobody asked for as
// unknown. Adding a field to a def = one Section call here + the TOML key.
#include "kfm/content/loader.hpp"

#include "embedded.hpp"
#include "kfm/content/decimal.hpp"
#include "kfm/sim/constants.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

namespace kfm::content {

using sim::AbilityDef;
using sim::AbilityKind;
using sim::ArenaDef;
using sim::ContentDB;
using sim::ContentName;
using sim::GrantKind;
using sim::ModeDef;
using sim::PickupSpawner;
using sim::PowerUpDef;
using sim::PowerUpGrant;
using sim::PowerUpShape;
using sim::StatKind;
using sim::Tag;
using sim::Tags;

// --- diagnostics ----------------------------------------------------------------

void Diagnostics::add(Severity severity, std::string_view file, uint32_t line,
                      std::string message) {
  if (severity == Severity::Error) ++errors_;
  if (items_.size() >= kMaxItems) {
    ++dropped_;
    return;
  }
  items_.push_back(ContentDiagnostic{severity, std::string(file), line, std::move(message)});
}

std::string Diagnostics::format() const {
  std::string out;
  for (const ContentDiagnostic& d : items_) {
    out += d.file.empty() ? std::string("content") : d.file;
    if (d.line != 0) out += ':' + std::to_string(d.line);
    out += d.severity == Severity::Error ? ": error: " : ": warning: ";
    out += d.message;
    out += '\n';
  }
  if (dropped_ != 0) out += "... and " + std::to_string(dropped_) + " more\n";
  return out;
}

namespace {

// --- name tables -------------------------------------------------------------------
// TOML spellings of the sim enums. A new AbilityKind or Tag needs its line
// here; the static_asserts fail until it has one.

struct AbilityKindName {
  std::string_view name;
  AbilityKind kind;
};
constexpr std::array<AbilityKindName, 2> kAbilityKinds = {{
    {"dash", AbilityKind::Dash},
    {"throw", AbilityKind::Throw},
}};
static_assert(kAbilityKinds.size() + 1 == static_cast<size_t>(AbilityKind::kCount),
              "every AbilityKind but None needs a TOML name in kAbilityKinds");

struct ModeKindName {
  std::string_view name;
  sim::ModeKind kind;
};
constexpr std::array<ModeKindName, 3> kModeKinds = {{
    {"rounds", sim::ModeKind::Rounds},
    {"deathmatch", sim::ModeKind::Deathmatch},
    {"survival", sim::ModeKind::Survival},
}};
static_assert(kModeKinds.size() == static_cast<size_t>(sim::ModeKind::kCount),
              "every ModeKind needs a TOML name in kModeKinds");

struct TagName {
  std::string_view name;
  Tag tag;
};
constexpr std::array<TagName, 6> kTagNames = {{
    {"dashing", Tag::Dashing},
    {"perfect_dash", Tag::PerfectDash},
    {"hit_stun", Tag::HitStun},
    {"shielded", Tag::Shielded},
    {"invulnerable", Tag::Invulnerable},
    {"eliminated", Tag::Eliminated},
}};
static_assert(kTagNames.size() == static_cast<size_t>(Tag::kCount),
              "every Tag needs a TOML name in kTagNames");

// Power-up spellings (§5.8).
struct StatName {
  std::string_view name;
  StatKind stat;
  bool integer;  // add-only
};
constexpr std::array<StatName, 3> kStatNames = {{
    {"move_speed", StatKind::MoveSpeed, false},
    {"max_hp", StatKind::MaxHp, true},
    {"ammo_max", StatKind::AmmoMax, true},
}};
static_assert(kStatNames.size() == static_cast<size_t>(StatKind::AmmoMax) + 1,
              "every StatKind needs a TOML name in kStatNames");

struct ShapeName {
  std::string_view name;
  PowerUpShape shape;
};
constexpr std::array<ShapeName, 4> kShapeNames = {{
    {"orb", PowerUpShape::Orb},
    {"cube", PowerUpShape::Cube},
    {"diamond", PowerUpShape::Diamond},
    {"coin", PowerUpShape::Coin},
}};
static_assert(kShapeNames.size() == static_cast<size_t>(PowerUpShape::Coin) + 1,
              "every PowerUpShape needs a TOML name in kShapeNames");

// Ability slots by the button that fires them (the [player] loadout order).
constexpr std::array<std::string_view, sim::kAbilitySlots> kSlotNames = {"dash", "throw",
                                                                         "ability"};

// Tags a power-up may grant: the rest are owned by AbilitySystem (dashing,
// perfect_dash) or by the damage rules (hit_stun, eliminated).
[[nodiscard]] constexpr bool grantableTag(Tag t) {
  return t == Tag::Shielded || t == Tag::Invulnerable;
}

// Projectile multipliers stay in (0, kMaxProjectileMul].
const Fixed kMaxProjectileMul = Fixed::fromInt(4);

[[nodiscard]] constexpr bool isWhole(Fixed f) {
  return f.raw % Fixed::kOneRaw == 0;
}
struct HazardKindName {
  std::string_view name;
  sim::HazardKind kind;
};
constexpr std::array<HazardKindName, 2> kHazardKinds = {{
    {"pit", sim::HazardKind::Pit},
    {"slow", sim::HazardKind::Slow},
}};
static_assert(kHazardKinds.size() == static_cast<size_t>(sim::HazardKind::kCount),
              "every HazardKind needs a TOML name in kHazardKinds");

// The entry of a name table whose `name` matches, or nullptr. A pointer rather
// than an iterator: std::array's iterator is a class on MSVC, so the
// clang-tidy-preferred `const auto*` would not compile there.
template <class Table>
[[nodiscard]] const typename Table::value_type* findNamed(const Table& table,
                                                          std::string_view name) {
  for (const auto& entry : table) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

// Name of the built-in inert ability at index 0.
constexpr std::string_view kNoneAbility = "none";

// Arena coordinates the swept narrowphase supports (collision_internal.hpp).
const Fixed kMaxArenaHalfExtent = Fixed::fromInt(1024);

[[nodiscard]] bool validName(std::string_view s) {
  if (s.empty() || s.size() > ContentName::kMaxChars) return false;
  return std::ranges::all_of(s, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

[[nodiscard]] std::string inQuotes(std::string_view s) {
  return '"' + std::string(s) + '"';
}

[[nodiscard]] std::string fixedText(Fixed f) {
  // Diagnostics only: a readable approximation is fine here.
  const int64_t milli = (static_cast<int64_t>(f.raw) * 1000) / Fixed::kOneRaw;
  const int64_t whole = milli / 1000;
  int64_t frac = milli % 1000;
  if (frac < 0) frac = -frac;
  const std::string out = (f.raw < 0 && whole == 0) ? "-0" : std::to_string(whole);
  const std::string fracText = std::to_string(frac);
  return out + '.' + std::string(3 - fracText.size(), '0') + fracText;
}

// --- per-file context ------------------------------------------------------------

struct File {
  std::string_view name;
  std::string_view text;
};

[[nodiscard]] uint32_t lineOf(const toml::node& n) {
  return n.source().begin.line;
}

// The literal text of a value, from its source position (1-based line and
// codepoint column, as toml++ counts them) to the next delimiter.
[[nodiscard]] std::string_view tokenAt(std::string_view text, const toml::source_position& pos) {
  size_t i = 0;
  for (uint32_t line = 1; line < pos.line && i < text.size(); ++i) {
    if (text[i] == '\n') ++line;
  }
  for (uint32_t col = 1; col < pos.column && i < text.size(); ++col) {
    // Skip one UTF-8 codepoint: a lead byte and its continuation bytes.
    ++i;
    while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0u) == 0x80u)
      ++i;
  }
  const size_t begin = i;
  while (i < text.size()) {
    const char c = text[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ']' || c == '}' ||
        c == '#') {
      break;
    }
    ++i;
  }
  return text.substr(begin, i - begin);
}

// Reads the keys of one table. `where` names it in messages ("[player]",
// "arena \"dojo\"").
class Section {
 public:
  Section(const toml::table& table, const File& file, Diagnostics& diag, std::string where)
      : table_(table), file_(file), diag_(diag), where_(std::move(where)) {}

  [[nodiscard]] uint32_t line() const { return lineOf(table_); }
  [[nodiscard]] const std::string& where() const { return where_; }
  [[nodiscard]] const File& file() const { return file_; }
  [[nodiscard]] Diagnostics& diag() const { return diag_; }
  void setWhere(std::string where) { where_ = std::move(where); }

  void error(const toml::node& at, const std::string& message) {
    diag_.error(file_.name, lineOf(at), where_ + ": " + message);
  }
  void error(const std::string& message) {
    diag_.error(file_.name, line(), where_ + ": " + message);
  }

  // The node for `key`, marking it used; nullptr when absent (an error when
  // required).
  const toml::node* take(std::string_view key, bool required = false) {
    used_.emplace_back(key);
    const toml::node* n = table_.get(key);
    if (n == nullptr && required) error("missing required key '" + std::string(key) + "'");
    return n;
  }

  // A Fixed from a decimal or integer literal, divided by `divisor` (60 for
  // per-second speeds), in [min, max].
  void fixed(std::string_view key, Fixed& out, Fixed min = Fixed::min(), Fixed max = Fixed::max(),
             bool required = false, int32_t divisor = 1) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return;
    if (const std::optional<Fixed> v = toFixed(*n, key, divisor)) {
      if (*v < min || *v > max) {
        error(*n, "'" + std::string(key) + "' = " + fixedText(*v) + " is outside [" +
                      fixedText(min) + ", " + fixedText(max) + "]");
        return;
      }
      out = *v;
    }
  }
  // Units per second in TOML, units per tick in the def.
  void speed(std::string_view key, Fixed& out, bool required = false) {
    fixed(key, out, Fixed::zero(), Fixed::max(), required, static_cast<int32_t>(sim::kTickHz));
  }

  template <class T>
  void integer(std::string_view key, T& out, int64_t min, int64_t max, bool required = false) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return;
    if (const std::optional<int64_t> v = toInteger(*n, key, min, max)) out = static_cast<T>(*v);
  }

  // A TOML boolean stored as a 0/1 switch.
  void flag(std::string_view key, uint8_t& out) {
    const toml::node* n = take(key);
    if (n == nullptr) return;
    if (const toml::value<bool>* b = n->as_boolean()) {
      out = b->get() ? 1 : 0;
    } else {
      error(*n, "'" + std::string(key) + "' must be true or false");
    }
  }

  // A string naming something; nullopt (with an error) when not a string.
  std::optional<std::string> string(std::string_view key, bool required = false) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return std::nullopt;
    if (const toml::value<std::string>* s = n->as_string()) return s->get();
    error(*n, "'" + std::string(key) + "' must be a string");
    return std::nullopt;
  }

  // The def's own name: required, lowercase identifier, fits ContentName.
  std::optional<std::string> name(ContentName& out) {
    const toml::node* n = take("name", true);
    if (n == nullptr) return std::nullopt;
    const toml::value<std::string>* s = n->as_string();
    if (s == nullptr || !validName(s->get())) {
      error(*n, "'name' must be 1 to " + std::to_string(ContentName::kMaxChars) +
                    " characters of a-z 0-9 _ -");
      return std::nullopt;
    }
    out = ContentName::of(s->get());
    return s->get();
  }

  // A string from a fixed name table; nullptr (with an error listing the
  // names) when it is not a string or not in the table, or absent.
  template <class Entry, size_t N, class Proj>
  const Entry* named(std::string_view key, const std::array<Entry, N>& table, Proj proj,
                     bool required = false) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return nullptr;
    if (const toml::value<std::string>* str = n->as_string()) {
      for (const Entry& e : table) {
        if (proj(e) == str->get()) return &e;
      }
    }
    std::string known;
    for (const Entry& e : table) {
      if (!known.empty()) known += ", ";
      known += proj(e);
    }
    error(*n, "'" + std::string(key) + "' must be one of: " + known);
    return nullptr;
  }

  // One tag name.
  std::optional<Tag> tag(std::string_view key, bool required = false) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return std::nullopt;
    if (const std::optional<Tag> t = tagNamed(*n)) return t;
    error(*n, "unknown tag in '" + std::string(key) + "' (known: " + tagList() + ")");
    return std::nullopt;
  }

  void tags(std::string_view key, Tags& out) {
    const toml::node* n = take(key);
    if (n == nullptr) return;
    const toml::array* arr = n->as_array();
    if (arr == nullptr) {
      error(*n, "'" + std::string(key) + "' must be a list of tag names");
      return;
    }
    Tags tags{};
    for (const toml::node& el : *arr) {
      const std::optional<Tag> tag = tagNamed(el);
      if (!tag) {
        error(el, "unknown tag in '" + std::string(key) + "' (known: " + tagList() + ")");
        continue;
      }
      tags.set(*tag);
    }
    out = tags;
  }

  // [x, y] of Fixed values.
  bool vec2(std::string_view key, Vec2& out, bool required = false) {
    const toml::node* n = take(key, required);
    if (n == nullptr) return false;
    return vec2Of(*n, key, out);
  }
  bool vec2Of(const toml::node& n, std::string_view what, Vec2& out) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() != 2) {
      error(n, "'" + std::string(what) + "' must be a pair [x, y]");
      return false;
    }
    const std::optional<Fixed> x = toFixed((*arr)[0], what, 1);
    const std::optional<Fixed> y = toFixed((*arr)[1], what, 1);
    if (!x || !y) return false;
    out = Vec2{*x, *y};
    return true;
  }

  std::optional<Fixed> toFixed(const toml::node& n, std::string_view key, int32_t divisor) {
    if (const toml::value<int64_t>* i = n.as_integer()) {
      const int64_t v = i->get();
      if (v > -32768 && v < 32768) {
        return Fixed::fromRaw(static_cast<int32_t>(v * Fixed::kOneRaw / divisor));
      }
    } else if (n.is_floating_point()) {
      const std::string_view token = tokenAt(file_.text, n.source().begin);
      if (const std::optional<Fixed> v = parseFixedDecimal(token, divisor)) return v;
      error(n, "'" + std::string(key) + "' = " + std::string(token) +
                   ": write a plain decimal with at most " + std::to_string(kMaxDecimalPlaces) +
                   " decimal places and magnitude below 32768");
      return std::nullopt;
    }
    error(n, "'" + std::string(key) + "' must be a number with magnitude below 32768");
    return std::nullopt;
  }

  std::optional<int64_t> toInteger(const toml::node& n, std::string_view key, int64_t min,
                                   int64_t max) {
    const toml::value<int64_t>* i = n.as_integer();
    if (i == nullptr || i->get() < min || i->get() > max) {
      error(n, "'" + std::string(key) + "' must be an integer in [" + std::to_string(min) + ", " +
                   std::to_string(max) + "]");
      return std::nullopt;
    }
    return i->get();
  }

  // Reports every key of the table no parser asked for.
  void finish() {
    for (auto&& [key, node] : table_) {
      if (std::ranges::find(used_, key.str()) == used_.end()) {
        diag_.error(file_.name, lineOf(node),
                    where_ + ": unknown key '" + std::string(key.str()) + "'");
      }
    }
  }

 private:
  static std::optional<Tag> tagNamed(const toml::node& n) {
    const toml::value<std::string>* s = n.as_string();
    if (s == nullptr) return std::nullopt;
    for (const TagName& t : kTagNames) {
      if (t.name == s->get()) return t.tag;
    }
    return std::nullopt;
  }

  static std::string tagList() {
    std::string s;
    for (const TagName& t : kTagNames) {
      if (!s.empty()) s += ", ";
      s += t.name;
    }
    return s;
  }

  const toml::table& table_;
  const File& file_;
  Diagnostics& diag_;
  std::string where_;
  std::vector<std::string_view> used_;
};

// --- the content set under construction -------------------------------------------

struct Origin {
  std::string file;
  uint32_t line{0};
};

class Builder {
 public:
  explicit Builder(Diagnostics& diag) : diag_(diag) {
    db_.abilities[0] = AbilityDef{};
    db_.abilities[0].name = ContentName::of(kNoneAbility);
    db_.abilityCount = 1;
  }

  void addDocument(const ContentSource& source) {
    toml::parse_result result = toml::parse(source.text, source.name);
    if (!result) {
      const toml::parse_error& e = result.error();
      diag_.error(source.name, e.source().begin.line,
                  "TOML syntax: " + std::string(e.description()));
      return;
    }
    const File file{source.name, source.text};
    const toml::table& root = result.table();
    for (auto&& [key, node] : root) {
      const std::string_view k = key.str();
      if (k == "player") {
        single(file, node, "[player]", playerAt_, [&](Section& s) { parsePlayer(s); });
      } else if (k == "boomerang") {
        single(file, node, "[boomerang]", boomerangAt_, [&](Section& s) { parseBoomerang(s); });
      } else if (k == "pickup") {
        single(file, node, "[pickup]", pickupAt_, [&](Section& s) { parsePickup(s); });
      } else if (k == "dash_attack") {
        single(file, node, "[dash_attack]", dashAttackAt_, [&](Section& s) { parseDashAttack(s); });
      } else if (k == "ability") {
        list(file, node, "ability", [&](Section& s) { parseAbility(s); });
      } else if (k == "arena") {
        list(file, node, "arena", [&](Section& s) { parseArena(s); });
      } else if (k == "mode") {
        list(file, node, "mode", [&](Section& s) { parseMode(s); });
      } else if (k == "powerup") {
        list(file, node, "powerup", [&](Section& s) { parsePowerUp(s); });
      } else {
        diag_.error(file.name, lineOf(node),
                    "unknown top-level key '" + std::string(k) +
                        "' (expected [player], [boomerang], [pickup], [dash_attack], "
                        "[[ability]], [[arena]], [[mode]] or [[powerup]])");
      }
    }
  }

  // Resolves cross-references, checks the set is complete and validates.
  bool finish(ContentDB& out) {
    requirePresent(playerAt_, "[player]");
    requirePresent(boomerangAt_, "[boomerang]");
    requirePresent(pickupAt_, "[pickup]");
    requirePresent(dashAttackAt_, "[dash_attack]");
    if (db_.arenaCount == 0) diag_.error("", 0, "no [[arena]] defined (need at least one)");
    if (db_.modeCount == 0) diag_.error("", 0, "no [[mode]] defined (need at least one)");
    resolveLoadouts();
    resolvePowerUpNames();
    if (diag_.hasErrors()) return false;
    if (!validateContent(db_, diag_)) return false;
    out = db_;
    return true;
  }

 private:
  struct PendingName {
    std::string name;
    std::string where;
    uint32_t line{0};
    std::string file;
  };
  struct SpawnerName {
    uint8_t arena{0};
    uint8_t spawner{0};
    PendingName name;
  };
  using Loadout = std::array<std::optional<PendingName>, sim::kAbilitySlots>;
  struct ModeLoadout {
    uint8_t mode{0};
    Loadout names;
  };

  template <class F>
  void single(const File& file, const toml::node& node, const char* what,
              std::optional<Origin>& seen, F&& parse) {
    const toml::table* t = node.as_table();
    if (t == nullptr) {
      diag_.error(file.name, lineOf(node), std::string(what) + " must be a table");
      return;
    }
    if (seen) {
      diag_.error(file.name, lineOf(node),
                  std::string(what) + " is defined again (first in " + seen->file + ":" +
                      std::to_string(seen->line) + ")");
      return;
    }
    seen = Origin{std::string(file.name), lineOf(node)};
    Section s(*t, file, diag_, what);
    parse(s);
    s.finish();
  }

  template <class F>
  void list(const File& file, const toml::node& node, const char* what, F&& parse) {
    const toml::array* arr = node.as_array();
    if (arr == nullptr || !arr->is_array_of_tables()) {
      diag_.error(file.name, lineOf(node),
                  "'" + std::string(what) + "' must be an array of tables: write [[" +
                      std::string(what) + "]]");
      return;
    }
    for (const toml::node& el : *arr) {
      Section s(*el.as_table(), file, diag_, std::string("[[") + what + "]]");
      parse(s);
      s.finish();
    }
  }

  void requirePresent(const std::optional<Origin>& seen, const char* what) {
    if (!seen) diag_.error("", 0, std::string("missing ") + what);
  }

  // A def name must be unique within its list.
  template <class Def, size_t N>
  bool uniqueName(Section& s, const std::string& name, const std::array<Def, N>& defs,
                  uint8_t count, const char* what) {
    for (uint8_t i = 0; i < count; ++i) {
      if (defs[i].name.view() == name) {
        s.error(std::string(what) + " " + inQuotes(name) + " is defined twice");
        return false;
      }
    }
    return true;
  }

  void parsePlayer(Section& s) {
    sim::PlayerDef& p = db_.player;
    s.fixed("radius", p.radius, Fixed::epsilon(), Fixed::fromInt(16), true);
    s.speed("move_speed", p.moveSpeed, true);
    s.integer("max_hp", p.maxHp, 1, 255, true);
    s.integer("ammo_max", p.ammoMax, 0, 255);
    s.integer("ammo_start", p.ammoStart, 0, 255);
    s.integer("hit_stun_ticks", p.hitStunTicks, 0, 65535);
    if (const toml::node* n = s.take("abilities", true)) parseLoadout(s, *n, loadout_);
  }

  // A loadout: 2 or 3 ability names, one per slot (dash button, throw button,
  // ability button); "none" leaves a slot empty, a missing third is "none".
  void parseLoadout(Section& s, const toml::node& n, Loadout& out) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() < 2 || arr->size() > sim::kAbilitySlots) {
      s.error(n, "'abilities' must list 2 or " + std::to_string(sim::kAbilitySlots) +
                     " ability names (one per slot: dash button, throw button, ability button)");
      return;
    }
    for (size_t i = 0; i < arr->size(); ++i) {
      const toml::node* item = arr->get(i);
      if (item == nullptr) continue;  // get() is nullable; GCC's -Wnull-dereference wants it seen
      const toml::value<std::string>* name = item->as_string();
      if (name == nullptr) {
        s.error(*item, "'abilities' entries must be ability names");
        continue;
      }
      out[i] = PendingName{name->get(), s.where(), lineOf(*item), std::string(s.file().name)};
    }
  }

  void parseBoomerang(Section& s) {
    sim::BoomerangDef& b = db_.boomerang;
    s.fixed("radius", b.radius, Fixed::epsilon(), Fixed::fromInt(16), true);
    s.speed("speed", b.speed, true);
    s.integer("outbound_ticks", b.outboundTicks, 0, 65535);
    s.integer("max_return_ticks", b.maxReturnTicks, 0, 65535);
    s.integer("turn_rate_bam", b.turnRateBam, 0, 32768);
    s.fixed("restitution", b.restitution, Fixed::zero(), Fixed::fromInt(4));
    s.fixed("spawn_offset", b.spawnOffset, Fixed::zero(), Fixed::fromInt(16));
    s.integer("damage", b.damage, 0, 255);
    s.flag("pierce", b.pierce);
  }

  void parsePickup(Section& s) {
    s.fixed("radius", db_.pickup.radius, Fixed::epsilon(), Fixed::fromInt(16), true);
  }

  void parseDashAttack(Section& s) {
    sim::DashAttackDef& d = db_.dashAttack;
    s.integer("damage", d.damage, 0, 255);
    s.integer("min_ammo", d.minAmmo, 0, 255);
    s.flag("consumes_ammo", d.consumesAmmo);
  }

  void parseAbility(Section& s) {
    AbilityDef d;
    const std::optional<std::string> name = s.name(d.name);
    if (name) s.setWhere("ability " + inQuotes(*name));
    if (const std::optional<std::string> kind = s.string("kind", true)) {
      const auto* const entry = findNamed(kAbilityKinds, *kind);
      if (entry == nullptr) {
        s.error("unknown kind " + inQuotes(*kind));
      } else {
        d.kind = entry->kind;
      }
    }
    s.integer("cooldown_ticks", d.cooldownTicks, 0, 65535);
    s.integer("startup_ticks", d.startupTicks, 0, 65535);
    s.integer("active_ticks", d.activeTicks, 0, 65535);
    s.integer("recovery_ticks", d.recoveryTicks, 0, 65535);
    s.integer("perfect_window_ticks", d.perfectWindowTicks, 0, 65535);
    s.speed("speed", d.speed);
    s.fixed("param0", d.param0);
    s.fixed("param1", d.param1);
    s.tags("tags_startup", d.tagsStartup);
    s.tags("tags_active", d.tagsActive);
    s.tags("tags_recovery", d.tagsRecovery);
    if (!name) return;
    if (*name == kNoneAbility) {
      s.error(inQuotes(kNoneAbility) + " is the built-in inert ability; pick another name");
      return;
    }
    if (!uniqueName(s, *name, db_.abilities, db_.abilityCount, "ability")) return;
    if (db_.abilityCount >= sim::kMaxAbilityDefs) {
      s.error("too many abilities (at most " + std::to_string(sim::kMaxAbilityDefs - 1) +
              ", kMaxAbilityDefs)");
      return;
    }
    db_.abilities[db_.abilityCount++] = d;
  }

  void parseArena(Section& s) {
    ArenaDef a;
    const std::optional<std::string> name = s.name(a.name);
    if (name) s.setWhere("arena " + inQuotes(*name));
    if (s.vec2("half_extents", a.halfExtents, true) &&
        (a.halfExtents.x <= Fixed::zero() || a.halfExtents.y <= Fixed::zero() ||
         a.halfExtents.x > kMaxArenaHalfExtent || a.halfExtents.y > kMaxArenaHalfExtent)) {
      s.error("'half_extents' must be in (0, 1024] on both axes");
    }
    const toml::node* points = s.take("spawn_points");
    const toml::node* ellipse = s.take("spawn_ellipse");
    if ((points == nullptr) == (ellipse == nullptr)) {
      s.error("give exactly one of 'spawn_points' or 'spawn_ellipse'");
    } else if (points != nullptr) {
      parseSpawnPoints(s, *points, a);
    } else {
      parseSpawnEllipse(s, *ellipse, a);
    }
    const toml::node* spawners = s.take("spawners");
    if (const toml::node* walls = s.take("walls")) parseWalls(s, *walls, a);
    if (const toml::node* hazards = s.take("hazards")) parseHazards(s, *hazards, a);
    if (!name || !uniqueName(s, *name, db_.arenas, db_.arenaCount, "arena")) return;
    if (db_.arenaCount >= sim::kMaxArenas) {
      s.error("too many arenas (at most " + std::to_string(sim::kMaxArenas) + ", kMaxArenas)");
      return;
    }
    if (spawners != nullptr) parseSpawners(s, *spawners, a, db_.arenaCount);
    db_.arenas[db_.arenaCount++] = a;
  }

  // spawners = [{ pos = [x, y], power_ups = ["name", ...], first_ticks,
  //               interval_ticks, max_alive }, ...] (§5.8). power_ups is
  // optional (every power-up); names resolve once every file is read.
  void parseSpawners(Section& outer, const toml::node& n, ArenaDef& a, uint8_t arenaIndex) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() > sim::kMaxPickupSpawners) {
      outer.error(n, "'spawners' must be a list of at most " +
                         std::to_string(sim::kMaxPickupSpawners) + " spawner tables");
      return;
    }
    for (size_t i = 0; i < arr->size(); ++i) {
      const toml::table* t = arr->get(i)->as_table();
      if (t == nullptr) {
        outer.error(*arr->get(i), "'spawners' entries must be tables { pos, ... }");
        continue;
      }
      Section s(*t, outer.file(), outer.diag(), outer.where() + " spawner " + std::to_string(i));
      PickupSpawner sp;
      s.vec2("pos", sp.pos, true);
      s.integer("first_ticks", sp.firstTicks, 0, 65535);
      s.integer("interval_ticks", sp.intervalTicks, 0, 65535);
      s.integer("max_alive", sp.maxAlive, 1, sim::kMaxPickups);
      if (const toml::node* set = s.take("power_ups")) {
        const toml::array* names = set->as_array();
        if (names == nullptr || names->empty()) {
          s.error(*set, "'power_ups' must be a non-empty list of power-up names");
        } else {
          for (const toml::node& el : *names) {
            const toml::value<std::string>* str = el.as_string();
            if (str == nullptr) {
              s.error(el, "'power_ups' entries must be power-up names");
              continue;
            }
            spawnerNames_.push_back(SpawnerName{
                arenaIndex, static_cast<uint8_t>(i),
                PendingName{str->get(), s.where(), lineOf(el), std::string(s.file().name)}});
          }
        }
      }
      s.finish();
      a.pickupSpawners[i] = sp;
    }
    a.pickupSpawnerCount = static_cast<uint8_t>(arr->size());
  }

  void parsePowerUp(Section& s) {
    PowerUpDef d;
    const std::optional<std::string> name = s.name(d.name);
    if (name) s.setWhere("powerup " + inQuotes(*name));
    s.integer("duration_ticks", d.durationTicks, 1, 65535, true);
    s.integer("color", d.visual.color, 0, sim::kMaxPlayers - 1);
    if (const ShapeName* shape =
            s.named("shape", kShapeNames, [](const ShapeName& e) { return e.name; })) {
      d.visual.shape = shape->shape;
    }
    const uint8_t index = db_.powerUpCount;
    if (const toml::node* grants = s.take("grants", true)) {
      const toml::array* arr = grants->as_array();
      if (arr == nullptr || arr->empty() || arr->size() > sim::kMaxPowerUpGrants) {
        s.error(*grants, "'grants' must list 1 to " + std::to_string(sim::kMaxPowerUpGrants) +
                             " grant tables");
      } else {
        for (size_t i = 0; i < arr->size(); ++i) {
          const toml::table* t = arr->get(i)->as_table();
          if (t == nullptr) {
            s.error(*arr->get(i), "'grants' entries must be tables { kind = ..., ... }");
            continue;
          }
          Section g(*t, s.file(), s.diag(), s.where() + " grant " + std::to_string(i));
          d.grants[i] = parseGrant(g, index, static_cast<uint8_t>(i));
          g.finish();
        }
        d.grantCount = static_cast<uint8_t>(arr->size());
      }
    }
    if (!name || !uniqueName(s, *name, db_.powerUps, db_.powerUpCount, "powerup")) return;
    if (db_.powerUpCount >= sim::kMaxPowerUps) {
      s.error("too many power-ups (at most " + std::to_string(sim::kMaxPowerUps) +
              ", kMaxPowerUps)");
      return;
    }
    db_.powerUps[db_.powerUpCount++] = d;
  }

  // One grant table: kind = "stat" | "ability" | "projectile" | "tag".
  PowerUpGrant parseGrant(Section& g, uint8_t powerUp, uint8_t grantIndex) {
    PowerUpGrant out;
    const std::optional<std::string> kind = g.string("kind", true);
    if (!kind) return out;
    if (*kind == "stat") {
      out.kind = GrantKind::Stat;
      const StatName* stat =
          g.named("stat", kStatNames, [](const StatName& e) { return e.name; }, true);
      if (stat != nullptr) out.stat.stat = stat->stat;
      g.fixed("mul", out.stat.mul, Fixed::zero(), Fixed::fromInt(16));
      g.fixed("add", out.stat.add, Fixed::fromInt(-255), Fixed::fromInt(255));
      if (stat != nullptr && stat->integer) {
        if (out.stat.mul != Fixed::one()) {
          g.error("'" + std::string(stat->name) +
                  "' is an integer stat: it takes 'add' only, never 'mul'");
        }
        if (!isWhole(out.stat.add)) {
          g.error("'" + std::string(stat->name) + "' is an integer stat: 'add' must be whole");
        }
      }
    } else if (*kind == "ability") {
      out.kind = GrantKind::AbilityOverride;
      const std::string_view* slot =
          g.named("slot", kSlotNames, [](std::string_view e) { return e; }, true);
      if (slot != nullptr) out.slot = static_cast<uint8_t>(slot - kSlotNames.data());
      if (const std::optional<std::string> ability = g.string("ability", true)) {
        overrideNames_.push_back(
            OverrideName{powerUp, grantIndex,
                         PendingName{*ability, g.where(), g.line(), std::string(g.file().name)}});
      }
    } else if (*kind == "projectile") {
      out.kind = GrantKind::Projectile;
      g.fixed("speed_mul", out.projectile.speedMul, Fixed::epsilon(), kMaxProjectileMul);
      g.fixed("radius_mul", out.projectile.radiusMul, Fixed::epsilon(), kMaxProjectileMul);
      g.integer("damage_add", out.projectile.damageAdd, 0, 255);
      g.flag("pierce", out.projectile.pierce);
    } else if (*kind == "tag") {
      out.kind = GrantKind::Tag;
      if (const std::optional<Tag> t = g.tag("tag", true)) {
        if (grantableTag(*t)) {
          out.tag = *t;
        } else {
          g.error("a power-up can grant only 'shielded' or 'invulnerable'");
        }
      }
    } else {
      g.error("unknown grant kind " + inQuotes(*kind) + " (stat, ability, projectile, tag)");
    }
    return out;
  }

  static void parseSpawnPoints(Section& s, const toml::node& n, ArenaDef& a) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->empty() || arr->size() > sim::kMaxPlayers) {
      s.error(n, "'spawn_points' must list 1 to " + std::to_string(sim::kMaxPlayers) +
                     " [x, y] points");
      return;
    }
    for (size_t i = 0; i < arr->size(); ++i) {
      if (!s.vec2Of((*arr)[i], "spawn_points", a.spawnPoints[i])) return;
    }
    a.spawnPointCount = static_cast<uint8_t>(arr->size());
  }

  // Points on an ellipse: point i at angle order[i] * step_degrees, through
  // the sim's fixed-point LUT trig (the same expression as the M1 tables).
  static void parseSpawnEllipse(Section& outer, const toml::node& n, ArenaDef& a) {
    const toml::table* t = n.as_table();
    if (t == nullptr) {
      outer.error(n, "'spawn_ellipse' must be a table { radii, step_degrees, order }");
      return;
    }
    // Messages and unknown keys of the inline table go through its own Section.
    Section s(*t, outer.file(), outer.diag(), outer.where() + " spawn_ellipse");
    Vec2 radii{};
    const bool haveRadii = s.vec2("radii", radii, true);
    uint16_t step = 0;
    s.integer("step_degrees", step, 1, 360, true);
    const toml::node* orderNode = s.take("order", true);
    s.finish();
    if (!haveRadii || step == 0 || orderNode == nullptr) return;
    const toml::array* order = orderNode->as_array();
    if (order == nullptr || order->empty() || order->size() > sim::kMaxPlayers) {
      s.error(*orderNode,
              "'order' must list 1 to " + std::to_string(sim::kMaxPlayers) + " step indices");
      return;
    }
    for (size_t i = 0; i < order->size(); ++i) {
      const std::optional<int64_t> k = s.toInteger((*order)[i], "order", 0, 359);
      if (!k) return;
      const Angle angle = Angle::fromDegrees(static_cast<int32_t>(*k) * step);
      a.spawnPoints[i] = Vec2{cos(angle) * radii.x, sin(angle) * radii.y};
    }
    a.spawnPointCount = static_cast<uint8_t>(order->size());
  }

  // `walls = [{ min = [x, y], max = [x, y] }, ...]` (v0.22): interior blocks.
  static void parseWalls(Section& outer, const toml::node& n, ArenaDef& a) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() > sim::kMaxArenaWalls) {
      outer.error(n, "'walls' must list at most " + std::to_string(sim::kMaxArenaWalls) +
                         " { min = [x, y], max = [x, y] } boxes (kMaxArenaWalls)");
      return;
    }
    uint8_t count = 0;
    for (const toml::node& el : *arr) {
      const toml::table* t = el.as_table();
      if (t == nullptr) {
        outer.error(el, "'walls' entries must be tables { min = [x, y], max = [x, y] }");
        return;
      }
      Section s(*t, outer.file(), outer.diag(), outer.where() + " wall " + std::to_string(count));
      sim::Wall& w = a.walls[count++];
      s.vec2("min", w.min, true);
      s.vec2("max", w.max, true);
      s.finish();
    }
    a.wallCount = count;
  }

  // `hazards = [{ kind = "pit" | "slow", min = [x, y], max = [x, y],
  // speed_mul = m }, ...]` (v0.22): speed_mul is for slow only.
  static void parseHazards(Section& outer, const toml::node& n, ArenaDef& a) {
    const toml::array* arr = n.as_array();
    if (arr == nullptr || arr->size() > sim::kMaxArenaHazards) {
      outer.error(n, "'hazards' must list at most " + std::to_string(sim::kMaxArenaHazards) +
                         " { kind, min, max } boxes (kMaxArenaHazards)");
      return;
    }
    uint8_t count = 0;
    for (const toml::node& el : *arr) {
      const toml::table* t = el.as_table();
      if (t == nullptr) {
        outer.error(el, "'hazards' entries must be tables { kind, min = [x, y], max = [x, y] }");
        return;
      }
      Section s(*t, outer.file(), outer.diag(), outer.where() + " hazard " + std::to_string(count));
      sim::Hazard& h = a.hazards[count++];
      if (const std::optional<std::string> kind = s.string("kind", true)) {
        bool known = false;
        for (const HazardKindName& k : kHazardKinds) {
          if (k.name == *kind) {
            h.kind = k.kind;
            known = true;
          }
        }
        if (!known) s.error("unknown hazard kind " + inQuotes(*kind) + " (known: pit, slow)");
      }
      s.vec2("min", h.min, true);
      s.vec2("max", h.max, true);
      if (h.kind == sim::HazardKind::Slow) {
        s.fixed("speed_mul", h.speedMul, Fixed::epsilon(), Fixed::fromInt(4), true);
      }
      s.finish();
    }
    a.hazardCount = count;
  }

  void parseMode(Section& s) {
    ModeDef m;
    const std::optional<std::string> name = s.name(m.name);
    if (name) s.setWhere("mode " + inQuotes(*name));
    if (const std::optional<std::string> kind = s.string("kind")) {
      const auto* const entry = findNamed(kModeKinds, *kind);
      if (entry == nullptr) {
        s.error("unknown kind " + inQuotes(*kind) + " (rounds, deathmatch or survival)");
      } else {
        m.kind = entry->kind;
      }
    }
    s.integer("rounds_to_win", m.roundsToWin, 1, 255);
    s.integer("round_end_ticks", m.roundEndTicks, 0, 65535);
    s.integer("round_start_ticks", m.roundStartTicks, 0, 65535);
    s.integer("time_limit_ticks", m.timeLimitTicks, 0, 4294967295);
    s.integer("score_to_win", m.scoreToWin, 0, 255);
    s.integer("respawn_ticks", m.respawnTicks, 0, 65535);
    s.integer("respawn_invuln_ticks", m.respawnInvulnTicks, 0, 65535);
    Loadout loadout{};
    if (const toml::node* n = s.take("abilities")) {
      parseLoadout(s, *n, loadout);
      m.hasAbilities = 1;
    }
    if (!name || !uniqueName(s, *name, db_.modes, db_.modeCount, "mode")) return;
    if (db_.modeCount >= sim::kMaxModes) {
      s.error("too many modes (at most " + std::to_string(sim::kMaxModes) + ", kMaxModes)");
      return;
    }
    if (m.hasAbilities != 0) modeLoadouts_.push_back(ModeLoadout{db_.modeCount, loadout});
    db_.modes[db_.modeCount++] = m;
  }

  // An ability name in a loadout: "none" is index 0.
  [[nodiscard]] std::optional<uint8_t> loadoutIndex(const PendingName& p) {
    if (p.name == kNoneAbility) return uint8_t{0};
    for (uint8_t i = 1; i < db_.abilityCount; ++i) {
      if (db_.abilities[i].name.view() == p.name) return i;
    }
    diag_.error(p.file, p.line,
                p.where + ": 'abilities' names unknown ability " + inQuotes(p.name));
    return std::nullopt;
  }

  void resolveLoadout(const Loadout& names, std::array<uint8_t, sim::kAbilitySlots>& out) {
    for (size_t slot = 0; slot < names.size(); ++slot) {
      if (!names[slot]) continue;
      if (const std::optional<uint8_t> i = loadoutIndex(*names[slot])) out[slot] = *i;
    }
  }

  void resolveLoadouts() {
    resolveLoadout(loadout_, db_.player.abilities);
    for (const ModeLoadout& m : modeLoadouts_) {
      sim::ModeDef& mode = db_.modes[m.mode];
      mode.abilities = {};
      resolveLoadout(m.names, mode.abilities);
    }
  }

  // Power-up names in spawner sets and ability names in overrides: every
  // file is read first, so a reference may point into a later file.
  void resolvePowerUpNames() {
    for (const SpawnerName& n : spawnerNames_) {
      if (const std::optional<uint8_t> i = db_.findPowerUp(n.name.name)) {
        PickupSpawner& sp = db_.arenas[n.arena].pickupSpawners[n.spawner];
        sp.powerUpSet = static_cast<uint16_t>(sp.powerUpSet | (1u << *i));
      } else {
        diag_.error(n.name.file, n.name.line,
                    n.name.where + ": 'power_ups' names unknown power-up " + inQuotes(n.name.name));
      }
    }
    for (const OverrideName& n : overrideNames_) {
      bool found = false;
      for (uint8_t i = 1; i < db_.abilityCount; ++i) {
        if (db_.abilities[i].name.view() == n.name.name) {
          if (n.powerUp < db_.powerUpCount) db_.powerUps[n.powerUp].grants[n.grant].ability = i;
          found = true;
        }
      }
      if (!found) {
        diag_.error(n.name.file, n.name.line,
                    n.name.where + ": 'ability' names unknown ability " + inQuotes(n.name.name));
      }
    }
  }

  struct OverrideName {
    uint8_t powerUp{0};
    uint8_t grant{0};
    PendingName name;
  };

  Diagnostics& diag_;
  ContentDB db_{};
  std::optional<Origin> playerAt_;
  std::optional<Origin> boomerangAt_;
  std::optional<Origin> pickupAt_;
  std::optional<Origin> dashAttackAt_;
  Loadout loadout_{};
  std::vector<ModeLoadout> modeLoadouts_;
  std::vector<SpawnerName> spawnerNames_;
  std::vector<OverrideName> overrideNames_;
};

}  // namespace

// --- public API ---------------------------------------------------------------------

bool parseContent(std::span<const ContentSource> sources, ContentDB& out, Diagnostics& diag) {
  const size_t errorsBefore = diag.errorCount();
  // A fresh Diagnostics for the set, appended to the caller's: finish()
  // must see only this load's errors.
  Diagnostics local;
  Builder b(local);
  for (const ContentSource& source : sources)
    b.addDocument(source);
  ContentDB db{};
  const bool ok = b.finish(db);
  for (const ContentDiagnostic& d : local.items())
    diag.add(d.severity, d.file, d.line, d.message);
  if (!ok || diag.errorCount() != errorsBefore) return false;
  out = db;
  return true;
}

bool parseContent(std::string_view tomlText, std::string_view sourceName, ContentDB& out,
                  Diagnostics& diag) {
  const ContentSource source{std::string(sourceName), std::string(tomlText)};
  return parseContent(std::span<const ContentSource>(&source, 1), out, diag);
}

namespace {

// Power-ups and spawners (§5.8): what ModifierSystem, the throw and the
// spawners rely on instead of re-checking per tick.
template <class Fail>
void validatePowerUps(const ContentDB& c, const Fail& fail) {
  if (c.powerUpCount > sim::kMaxPowerUps) {
    fail("power-up count " + std::to_string(c.powerUpCount) + " above kMaxPowerUps");
    return;
  }
  for (uint8_t i = 0; i < c.powerUpCount; ++i) {
    const PowerUpDef& d = c.powerUps[i];
    const std::string where = "powerup " + inQuotes(d.name.view());
    if (!validName(d.name.view())) fail(where + ": invalid name");
    for (uint8_t j = 0; j < i; ++j) {
      if (c.powerUps[j].name == d.name) fail(where + ": name used twice");
    }
    if (d.durationTicks < 1) fail(where + ": duration_ticks must be at least 1");
    if (d.visual.color >= sim::kMaxPlayers) fail(where + ": color must be a palette index 0-9");
    if (d.grantCount < 1 || d.grantCount > sim::kMaxPowerUpGrants) {
      fail(where + ": needs 1 to " + std::to_string(sim::kMaxPowerUpGrants) + " grants");
      continue;
    }
    for (uint8_t g = 0; g < d.grantCount; ++g) {
      const PowerUpGrant& grant = d.grants[g];
      const std::string at = where + " grant " + std::to_string(g);
      switch (grant.kind) {
        case GrantKind::Stat:
          if (grant.stat.stat != StatKind::MoveSpeed && grant.stat.mul != Fixed::one()) {
            fail(at + ": an integer stat takes 'add' only, never 'mul'");
          }
          if (grant.stat.stat != StatKind::MoveSpeed && !isWhole(grant.stat.add)) {
            fail(at + ": an integer stat's 'add' must be whole");
          }
          if (grant.stat.mul < Fixed::zero()) fail(at + ": 'mul' must not be negative");
          break;
        case GrantKind::AbilityOverride: {
          if (grant.slot >= sim::kAbilitySlots) {
            fail(at + ": slot " + std::to_string(grant.slot) + " is not an ability slot");
            break;
          }
          if (grant.ability < 1 || grant.ability >= c.abilityCount) {
            fail(at + ": ability index " + std::to_string(grant.ability) + " is not defined");
            break;
          }
          // Buttons belong to slots (v0.21): any kind may run in any slot.
          break;
        }
        case GrantKind::Projectile:
          if (grant.projectile.speedMul <= Fixed::zero() ||
              grant.projectile.speedMul > kMaxProjectileMul ||
              grant.projectile.radiusMul <= Fixed::zero() ||
              grant.projectile.radiusMul > kMaxProjectileMul) {
            fail(at + ": speed_mul and radius_mul must be in (0, 4]");
          }
          if (grant.projectile.pierce > 1) fail(at + ": pierce is a 0/1 switch");
          break;
        case GrantKind::Tag:
          if (!grantableTag(grant.tag)) fail(at + ": only shielded or invulnerable");
          break;
        case GrantKind::None:
          fail(at + ": grant has no kind");
          break;
      }
    }
  }

  for (uint8_t i = 0; i < c.arenaCount && i < sim::kMaxArenas; ++i) {
    const ArenaDef& a = c.arenas[i];
    const std::string where = "arena " + inQuotes(a.name.view());
    if (a.pickupSpawnerCount > sim::kMaxPickupSpawners) {
      fail(where + ": more than " + std::to_string(sim::kMaxPickupSpawners) + " spawners");
      continue;
    }
    if (a.pickupSpawnerCount > 0 && c.powerUpCount == 0) {
      fail(where + ": has spawners but no [[powerup]] is defined");
    }
    uint32_t alive = 0;
    const uint32_t defined = c.powerUpCount >= 16 ? 0xFFFFu : (uint32_t{1} << c.powerUpCount) - 1u;
    for (uint8_t k = 0; k < a.pickupSpawnerCount; ++k) {
      const PickupSpawner& sp = a.pickupSpawners[k];
      const std::string at = where + " spawner " + std::to_string(k);
      alive += sp.maxAlive;
      if (sp.maxAlive < 1) fail(at + ": max_alive must be at least 1");
      if ((sp.powerUpSet & ~defined) != 0) fail(at + ": names a power-up that is not defined");
      const Fixed limitX = a.halfExtents.x - c.pickup.radius;
      const Fixed limitY = a.halfExtents.y - c.pickup.radius;
      if (abs(sp.pos.x) > limitX || abs(sp.pos.y) > limitY) {
        fail(at + ": pos (" + fixedText(sp.pos.x) + ", " + fixedText(sp.pos.y) +
             ") is outside the arena");
      }
      // A pickup inside a wall could never be reached; one over a pit would
      // drop whoever took it (v0.22 statics).
      for (uint8_t w = 0; w < a.wallCount && w < sim::kMaxArenaWalls; ++w) {
        if (sim::circleOverlapsBox(a.walls[w].min, a.walls[w].max, sp.pos, c.pickup.radius)) {
          fail(at + ": pos (" + fixedText(sp.pos.x) + ", " + fixedText(sp.pos.y) +
               ") is inside wall " + std::to_string(w));
        }
      }
      for (uint8_t h = 0; h < a.hazardCount && h < sim::kMaxArenaHazards; ++h) {
        const sim::Hazard& hz = a.hazards[h];
        if (hz.kind == sim::HazardKind::Pit && sim::boxContains(hz.min, hz.max, sp.pos)) {
          fail(at + ": pos (" + fixedText(sp.pos.x) + ", " + fixedText(sp.pos.y) +
               ") is in pit hazard " + std::to_string(h));
        }
      }
    }
    // Power-ups never take more than half the pickup pool: grounded bananas
    // need the rest (a full pool makes a grounded banana vanish).
    if (alive > sim::kMaxPickups / 2) {
      fail(where + ": spawners' max_alive add up to " + std::to_string(alive) + " (at most " +
           std::to_string(sim::kMaxPickups / 2) + ")");
    }
  }
}

[[nodiscard]] std::string pointText(Vec2 p) {
  return "(" + fixedText(p.x) + ", " + fixedText(p.y) + ")";
}

// Walls and hazards of one arena (v0.22): counts in range, boxes non-empty
// and inside the arena, and no spawn point that starts a monkey overlapping
// a wall or standing in a pit.
template <class Fail>
void validateStatics(const ContentDB& c, const ArenaDef& a, const std::string& where,
                     const Fail& fail) {
  if (a.wallCount > sim::kMaxArenaWalls || a.hazardCount > sim::kMaxArenaHazards) {
    fail(where + ": more walls or hazards than kMaxArenaWalls / kMaxArenaHazards");
    return;
  }
  const auto checkBox = [&](const std::string& what, Vec2 min, Vec2 max) {
    if (!(min.x < max.x && min.y < max.y)) {
      fail(where + ": " + what + " needs min < max on both axes, got min " + pointText(min) +
           " max " + pointText(max));
    }
    if (min.x < -a.halfExtents.x || min.y < -a.halfExtents.y || max.x > a.halfExtents.x ||
        max.y > a.halfExtents.y) {
      fail(where + ": " + what + " reaches outside the arena box");
    }
  };
  for (uint8_t w = 0; w < a.wallCount; ++w) {
    checkBox("wall " + std::to_string(w), a.walls[w].min, a.walls[w].max);
  }
  for (uint8_t h = 0; h < a.hazardCount; ++h) {
    const sim::Hazard& hz = a.hazards[h];
    const std::string what = "hazard " + std::to_string(h);
    checkBox(what, hz.min, hz.max);
    std::string prefix = where;
    prefix += ": ";
    prefix += what;
    if (hz.kind >= sim::HazardKind::kCount) fail(prefix + " has an unknown kind");
    if (hz.kind == sim::HazardKind::Slow &&
        (hz.speedMul <= Fixed::zero() || hz.speedMul > Fixed::fromInt(4))) {
      fail(prefix + " speed_mul must be in (0, 4]");
    }
  }
  for (uint8_t k = 0; k < a.spawnPointCount && k < sim::kMaxPlayers; ++k) {
    const Vec2 p = a.spawnPoints[k];
    for (uint8_t w = 0; w < a.wallCount; ++w) {
      if (sim::circleOverlapsBox(a.walls[w].min, a.walls[w].max, p, c.player.radius)) {
        fail(where + ": spawn point " + std::to_string(k) + " " + pointText(p) +
             " puts a monkey inside wall " + std::to_string(w));
      }
    }
    for (uint8_t h = 0; h < a.hazardCount; ++h) {
      const sim::Hazard& hz = a.hazards[h];
      if (hz.kind == sim::HazardKind::Pit && sim::boxContains(hz.min, hz.max, p)) {
        fail(where + ": spawn point " + std::to_string(k) + " " + pointText(p) +
             " is in pit hazard " + std::to_string(h));
      }
    }
  }
}

}  // namespace

bool validateContent(const ContentDB& c, Diagnostics& diag) {
  const size_t errorsBefore = diag.errorCount();
  const auto fail = [&](const std::string& message) { diag.error("", 0, message); };

  if (c.abilityCount < 1 || c.abilityCount > sim::kMaxAbilityDefs) {
    fail("ability count " + std::to_string(c.abilityCount) + " outside [1, kMaxAbilityDefs]");
    return false;
  }
  if (c.abilities[0].kind != AbilityKind::None) fail("ability 0 must be the inert none ability");
  for (uint8_t i = 1; i < c.abilityCount; ++i) {
    const AbilityDef& d = c.abilities[i];
    const std::string where = "ability " + inQuotes(d.name.view());
    if (d.kind == AbilityKind::None || d.kind >= AbilityKind::kCount) {
      fail(where + ": kind must be a real ability kind");
    }
    // PerfectDash is granted from the press tick (§5.5) and the perfect
    // window, plus its sub-tick tail, fits inside the active phase.
    if (d.kind == AbilityKind::Dash && d.startupTicks != 0) {
      fail(where + ": a dash must have startup_ticks = 0 (PerfectDash starts at the press)");
    }
    if (d.kind == AbilityKind::Dash && d.activeTicks <= d.perfectWindowTicks) {
      fail(where + ": active_ticks must exceed perfect_window_ticks");
    }
    for (uint8_t j = 0; j < i; ++j) {
      if (c.abilities[j].name == d.name) fail(where + ": name used twice");
    }
  }

  // Loadouts (v0.21): buttons belong to slots, so any kind may sit in any
  // slot, but the dash and throw buttons always fire something; the ability
  // button's slot may stay "none".
  const auto checkLoadout = [&](const std::array<uint8_t, sim::kAbilitySlots>& slots,
                                const std::string& where) {
    for (uint8_t slot = 0; slot < sim::kAbilitySlots; ++slot) {
      if (slots[slot] >= c.abilityCount) {
        fail(where + " abilities: slot " + std::to_string(slot) + " names an undefined ability");
      } else if (slot != sim::kAbilitySlotExtra && slots[slot] == 0) {
        fail(where + " abilities: the " + std::string(kSlotNames[slot]) +
             " slot must hold an ability (only the third may be none)");
      }
    }
  };
  checkLoadout(c.player.abilities, "[player]");
  for (uint8_t i = 0; i < c.modeCount && i < sim::kMaxModes; ++i) {
    if (c.modes[i].hasAbilities > 1)
      fail("mode " + inQuotes(c.modes[i].name.view()) + ": bad flag");
    if (c.modes[i].hasAbilities == 1) {
      checkLoadout(c.modes[i].abilities, "mode " + inQuotes(c.modes[i].name.view()));
    }
  }
  if (c.player.radius <= Fixed::zero()) fail("[player] radius must be positive");
  if (c.player.maxHp < 1) fail("[player] max_hp must be at least 1");
  if (c.player.ammoStart > c.player.ammoMax) fail("[player] ammo_start exceeds ammo_max");
  // A thrown banana spawns clear of its thrower (§5.7).
  if (c.boomerang.spawnOffset <= c.player.radius + c.boomerang.radius) {
    fail("[boomerang] spawn_offset must exceed player radius + boomerang radius (" +
         fixedText(c.player.radius + c.boomerang.radius) + ")");
  }
  if (c.boomerang.pierce > 1) fail("[boomerang] pierce is a 0/1 switch");
  // The default dash attack needs a banana in hand (§5.6); 0 would make
  // every dash an attack.
  if (c.dashAttack.minAmmo < 1) fail("[dash_attack] min_ammo must be at least 1");
  if (c.dashAttack.consumesAmmo > 1) fail("[dash_attack] consumes_ammo is a 0/1 switch");

  if (c.arenaCount < 1 || c.arenaCount > sim::kMaxArenas) {
    fail("arena count " + std::to_string(c.arenaCount) + " outside [1, kMaxArenas]");
  }
  for (uint8_t i = 0; i < c.arenaCount && i < sim::kMaxArenas; ++i) {
    const ArenaDef& a = c.arenas[i];
    const std::string where = "arena " + inQuotes(a.name.view());
    if (!validName(a.name.view())) fail(where + ": invalid name");
    for (uint8_t j = 0; j < i; ++j) {
      if (c.arenas[j].name == a.name) fail(where + ": name used twice");
    }
    if (a.halfExtents.x <= Fixed::zero() || a.halfExtents.y <= Fixed::zero() ||
        a.halfExtents.x > kMaxArenaHalfExtent || a.halfExtents.y > kMaxArenaHalfExtent) {
      fail(where + ": half_extents must be in (0, 1024]");
    }
    if (a.spawnPointCount < 1 || a.spawnPointCount > sim::kMaxPlayers) {
      fail(where + ": needs 1 to " + std::to_string(sim::kMaxPlayers) + " spawn points");
      continue;
    }
    const Fixed limitX = a.halfExtents.x - c.player.radius;
    const Fixed limitY = a.halfExtents.y - c.player.radius;
    for (uint8_t k = 0; k < a.spawnPointCount; ++k) {
      const Vec2 p = a.spawnPoints[k];
      if (abs(p.x) > limitX || abs(p.y) > limitY) {
        fail(where + ": spawn point " + std::to_string(k) + " (" + fixedText(p.x) + ", " +
             fixedText(p.y) + ") puts a monkey outside the arena");
      }
    }
    validateStatics(c, a, where, fail);
  }

  if (c.modeCount < 1 || c.modeCount > sim::kMaxModes) {
    fail("mode count " + std::to_string(c.modeCount) + " outside [1, kMaxModes]");
  }
  for (uint8_t i = 0; i < c.modeCount && i < sim::kMaxModes; ++i) {
    const ModeDef& m = c.modes[i];
    const std::string where = "mode " + inQuotes(m.name.view());
    if (!validName(m.name.view())) fail(where + ": invalid name");
    for (uint8_t j = 0; j < i; ++j) {
      if (c.modes[j].name == m.name) fail(where + ": name used twice");
    }
    if (m.kind >= sim::ModeKind::kCount) fail(where + ": kind must be a real mode kind");
    if (m.roundsToWin < 1) fail(where + ": rounds_to_win must be at least 1");
    if (m.kind == sim::ModeKind::Deathmatch) {
      if (m.scoreToWin < 1) fail(where + ": score_to_win must be at least 1 in deathmatch");
      if (m.respawnTicks < 1) fail(where + ": respawn_ticks must be at least 1 in deathmatch");
    }
  }
  validatePowerUps(c, fail);
  return diag.errorCount() == errorsBefore;
}

namespace {

[[nodiscard]] bool isTomlFile(const std::filesystem::directory_entry& e) {
  std::error_code ec;
  return e.is_regular_file(ec) && e.path().extension() == ".toml";
}

}  // namespace

bool readContentDir(const std::filesystem::path& dir, std::vector<ContentSource>& out,
                    Diagnostics& diag) {
  const std::string dirName = dir.generic_string();
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    diag.error(dirName, 0, "not a content directory");
    return false;
  }
  std::vector<std::pair<std::string, std::filesystem::path>> files;
  for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!isTomlFile(*it)) continue;
    files.emplace_back(std::filesystem::relative(it->path(), dir, ec).generic_string(), it->path());
  }
  if (ec) {
    diag.error(dirName, 0, "cannot list: " + ec.message());
    return false;
  }
  if (files.empty()) {
    diag.error(dirName, 0, "no *.toml content files");
    return false;
  }
  std::ranges::sort(files);  // canonical order: sorted relative paths
  std::vector<ContentSource> sources;
  sources.reserve(files.size());
  for (const auto& [name, path] : files) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
      diag.error(name, 0, "cannot read " + path.generic_string());
      return false;
    }
    sources.push_back(ContentSource{
        name, std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>())});
  }
  out = std::move(sources);
  return true;
}

bool loadContentDir(const std::filesystem::path& dir, ContentDB& out, Diagnostics& diag) {
  std::vector<ContentSource> sources;
  if (!readContentDir(dir, sources, diag)) return false;
  return parseContent(sources, out, diag);
}

std::span<const ContentSource> embeddedContentSources() {
  static const std::vector<ContentSource> kSources = [] {
    std::vector<ContentSource> v;
    for (const detail::EmbeddedFile& f : detail::embeddedFiles()) {
      v.push_back(ContentSource{std::string(f.path), std::string(f.text())});
    }
    return v;
  }();
  return kSources;
}

bool loadEmbeddedContent(ContentDB& out, Diagnostics& diag) {
  return parseContent(embeddedContentSources(), out, diag);
}

uint64_t contentDirStamp(const std::filesystem::path& dir) {
  std::error_code ec;
  uint64_t stamp = 0;
  uint64_t count = 0;
  for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!isTomlFile(*it)) continue;
    std::error_code fileEc;
    const auto mtime = std::filesystem::last_write_time(it->path(), fileEc);
    const auto size = std::filesystem::file_size(it->path(), fileEc);
    if (fileEc) continue;
    const auto ticks = static_cast<uint64_t>(mtime.time_since_epoch().count());
    // Order-independent fold: the iteration order is unspecified.
    stamp += (ticks ^ (static_cast<uint64_t>(size) * 0x9E37'79B9'7F4A'7C15ULL)) *
             0xBF58'476D'1CE4'E5B9ULL;
    ++count;
  }
  if (ec) return 0;
  return stamp ^ (count * 0x94D0'49BB'1331'11EBULL);
}

}  // namespace kfm::content
