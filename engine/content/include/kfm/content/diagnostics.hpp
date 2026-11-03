// Content loader diagnostics (architecture §5.10): what went wrong, where.
// The loader never throws and never aborts; it reports through this list
// and leaves the output ContentDB untouched on error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kfm::content {

enum class Severity : uint8_t { Warning = 0, Error };

struct ContentDiagnostic {
  Severity severity{Severity::Error};
  std::string file;  // source name as given to the loader (a relative path)
  uint32_t line{0};  // 1-based; 0 = the whole file or the whole content set
  std::string message;
};

// A bounded list: past kMaxItems further diagnostics are only counted, so a
// badly broken file cannot flood the console.
class Diagnostics {
 public:
  static constexpr size_t kMaxItems = 64;

  void add(Severity severity, std::string_view file, uint32_t line, std::string message);
  void error(std::string_view file, uint32_t line, std::string message) {
    add(Severity::Error, file, line, std::move(message));
  }
  void warning(std::string_view file, uint32_t line, std::string message) {
    add(Severity::Warning, file, line, std::move(message));
  }

  [[nodiscard]] const std::vector<ContentDiagnostic>& items() const { return items_; }
  [[nodiscard]] size_t errorCount() const { return errors_; }
  [[nodiscard]] bool hasErrors() const { return errors_ != 0; }
  [[nodiscard]] size_t dropped() const { return dropped_; }
  void clear() { *this = Diagnostics{}; }

  // "file:line: error: message" lines, compiler style, one per diagnostic.
  [[nodiscard]] std::string format() const;

 private:
  std::vector<ContentDiagnostic> items_;
  size_t errors_{0};
  size_t dropped_{0};
};

}  // namespace kfm::content
