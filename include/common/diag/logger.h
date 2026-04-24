#pragma once

#include <string_view>

namespace common {

enum class LogLevel {
  Error,
  Warn,
  Info,
  Debug,
  Trace,
};

class Logger {
public:
  virtual ~Logger() = default;

  [[nodiscard]] virtual bool enabled(LogLevel level) const   = 0;
  virtual void log(LogLevel level, std::string_view message) = 0;
};

class NullLogger final : public Logger {
public:
  [[nodiscard]] bool enabled(LogLevel) const override { return false; }

  void log(LogLevel, std::string_view) override {}
};

} // namespace common
