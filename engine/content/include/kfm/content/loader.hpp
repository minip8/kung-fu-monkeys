// engine/content: TOML content files -> validated sim::ContentDB
// (architecture §5.10). Sits beside net/ai/presentation above sim; the only
// engine module that reads content files. The sim never does file IO.
//
// A content set is any number of TOML documents merged in a canonical order
// (sorted relative paths). Top-level keys:
//   [player] [boomerang] [pickup] [dash_attack]   exactly once in the set
//   [[ability]] [[arena]] [[mode]]                appended in load order
// Defs refer to each other by name (the player's loadout names abilities);
// the loader resolves names to indices. Unknown keys are errors, so a typo
// never silently falls back to a default. Fixed-point values are written as
// decimals and converted exactly (decimal.hpp), never through double.
#pragma once

#include "kfm/content/diagnostics.hpp"
#include "kfm/sim/content.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kfm::content {

// One content document: its source name (a relative path such as
// "arenas.toml", used in diagnostics and for ordering) and its text.
struct ContentSource {
  std::string name;
  std::string text;
};

// Parses and merges the documents in the order given, resolves names and
// validates (validateContent). Pure: no file IO. On success writes `out`
// and returns true; on any error leaves `out` untouched and returns false.
// Warnings never fail a load.
[[nodiscard]] bool parseContent(std::span<const ContentSource> sources, sim::ContentDB& out,
                                Diagnostics& diag);
// One document that holds a complete content set.
[[nodiscard]] bool parseContent(std::string_view tomlText, std::string_view sourceName,
                                sim::ContentDB& out, Diagnostics& diag);

// Structural checks on a built DB, shared by the loader and the tests of
// defaultContent(): capacities, at least one arena and mode, the base
// loadout's kinds, the dash's perfect window, arena bounds and spawn
// points, banana spawn clearance, dash attack switches.
[[nodiscard]] bool validateContent(const sim::ContentDB& content, Diagnostics& diag);

// Every *.toml under `dir` (recursively), read and sorted by relative path
// with '/' separators, then parseContent. Missing or empty directories are
// errors.
[[nodiscard]] bool readContentDir(const std::filesystem::path& dir, std::vector<ContentSource>& out,
                                  Diagnostics& diag);
[[nodiscard]] bool loadContentDir(const std::filesystem::path& dir, sim::ContentDB& out,
                                  Diagnostics& diag);

// The content/ directory of the source tree, compiled into the binary at
// build time (embedded.cpp, generated from content/*.toml): every host's
// fallback and the default of kfm_replay and the browser client.
[[nodiscard]] std::span<const ContentSource> embeddedContentSources();
[[nodiscard]] bool loadEmbeddedContent(sim::ContentDB& out, Diagnostics& diag);

// Newest modification time over the *.toml files under `dir` plus their
// count folded in, so adding or deleting a file also changes it; 0 when the
// directory cannot be read. For hot reload polling.
[[nodiscard]] uint64_t contentDirStamp(const std::filesystem::path& dir);

}  // namespace kfm::content
