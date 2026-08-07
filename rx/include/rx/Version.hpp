#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace rx {
enum class VersionTag { Draft, RC, Release };

struct Version {
  std::uint32_t raw{};     // commit date packed as YYYYMMDD
  std::uint32_t rawTime{}; // commit time-of-day packed as HHMM (orders same-day builds)
  VersionTag tag{};
  std::uint32_t tagVersion{};
  std::uint32_t gitTag{};
  bool dirty{};

  // Short git revision (7 hex chars), or empty if unknown. Precise, but not
  // ordered — kept for bug reports in logs, not shown in the user-facing name.
  std::string gitRev() const {
    if (gitTag == 0) {
      return {};
    }

    auto value = gitTag;
    char buf[7];
    for (int i = 0; i < 7; ++i) {
      auto digit = value & 0xf;
      value >>= 4;
      buf[i] = digit >= 10 ? static_cast<char>('a' + (digit - 10))
                           : static_cast<char>('0' + digit);
    }

    std::string result;
    for (int i = 0; i < 7; ++i) {
      result += buf[6 - i];
    }
    return result;
  }

  // Human-friendly, strictly-ordered name: calendar date + commit time-of-day,
  // e.g. "2026.06.06-0930". A later value is unambiguously the newer build
  // (which a git hash can't convey); the exact hash is logged via gitRev().
  std::string toString() const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04u.%02u.%02u-%02u%02u",
                  static_cast<unsigned>(raw / 10000u),
                  static_cast<unsigned>((raw / 100u) % 100u),
                  static_cast<unsigned>(raw % 100u),
                  static_cast<unsigned>(rawTime / 100u),
                  static_cast<unsigned>(rawTime % 100u));
    std::string result = buf;

    switch (tag) {
    case VersionTag::Draft:
      result += " Draft";
      break;
    case VersionTag::RC:
      result += " RC";
      break;
    case VersionTag::Release:
      break;
    }

    if (tagVersion) {
      result += std::to_string(tagVersion);
    }

    if (dirty) {
      result += '+';
    }

    return result;
  }
};

Version getVersion();
} // namespace rx
