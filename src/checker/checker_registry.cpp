#include "checker/checker_registry.h"

#include <memory>

#include "checker/basic/co_snapshot_checker.h"
#include "checker/so_only/so_only_checker.h"

namespace checker {

void CheckerRegistry::register_checker(std::unique_ptr<Checker> checker) {
  checkers_.push_back(std::move(checker));
}

const Checker *CheckerRegistry::find(std::string_view name) const {
  for (const auto &checker : checkers_) {
    if (checker->name() == name) {
      return checker.get();
    }
  }
  return nullptr;
}

std::vector<std::string_view> CheckerRegistry::names() const {
  std::vector<std::string_view> result;
  result.reserve(checkers_.size());
  for (const auto &checker : checkers_) {
    result.push_back(checker->name());
  }
  return result;
}

const std::vector<std::unique_ptr<Checker>> &CheckerRegistry::checkers() const {
  return checkers_;
}

CheckerRegistry make_default_checker_registry() {
  CheckerRegistry registry;
  registry.register_checker(std::make_unique<CoSnapshotBasicChecker>());
  registry.register_checker(std::make_unique<SnapshotSOChecker>());
  return registry;
}

} // namespace checker
