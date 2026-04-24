#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "co_checker/checker/checker.h"

namespace co_checker {

class CheckerRegistry {
public:
    void register_checker(std::unique_ptr<Checker> checker);

    [[nodiscard]] const Checker* find(std::string_view name) const;
    [[nodiscard]] std::vector<std::string_view> names() const;
    [[nodiscard]] const std::vector<std::unique_ptr<Checker>>& checkers() const;

private:
    std::vector<std::unique_ptr<Checker>> checkers_;
};

[[nodiscard]] CheckerRegistry make_default_checker_registry();

}  // namespace co_checker
