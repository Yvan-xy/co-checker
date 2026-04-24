#pragma once

#include <optional>
#include <utility>

#include "co_checker/diag/diagnostic.h"

namespace co_checker {

template <typename T>
class Result {
public:
    static Result ok(T value) {
        Result result;
        result.value_ = std::move(value);
        return result;
    }

    static Result err(DiagnosticList diagnostics) {
        Result result;
        result.diagnostics_ = std::move(diagnostics);
        return result;
    }

    [[nodiscard]] bool has_value() const {
        return value_.has_value();
    }

    explicit operator bool() const {
        return has_value();
    }

    [[nodiscard]] const T& value() const {
        return *value_;
    }

    [[nodiscard]] T& value() {
        return *value_;
    }

    [[nodiscard]] const DiagnosticList& error() const {
        return diagnostics_;
    }

private:
    std::optional<T> value_;
    DiagnosticList diagnostics_;
};

}  // namespace co_checker
