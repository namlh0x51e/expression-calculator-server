#pragma once

#include <spdlog/spdlog.h>

#include <expected>
#include <functional>
#include <span>
#include <stack>
#include <variant>

#include "tokenizer.hpp"

struct EvalErrorDividedByZero {};
struct EvalErrorInsufficientOperandsAmount {};
struct EvalErrorInvalidSyntax {};

using EvalError = std::variant<EvalErrorDividedByZero, EvalErrorInsufficientOperandsAmount, EvalErrorInvalidSyntax>;

class EvaluationEngine {
   public:
    using on_done_cb_t = std::function<void(std::expected<int64_t, EvalError>)>;

    [[nodiscard]]
    auto eval(std::span<std::byte> buffer) noexcept -> std::size_t;
    auto on_done(on_done_cb_t) noexcept -> void;

   private:
    static constexpr auto get_precedence(char op) noexcept -> uint16_t;
    static constexpr auto apply(char op, int64_t v1, int64_t v2) noexcept
        -> std::expected<int64_t, EvalErrorDividedByZero>;

    auto pop_operator_and_eval() noexcept -> std::expected<void, EvalError>;

   private:
    std::stack<int64_t> value_stack_;
    std::stack<char> op_stack_;
    on_done_cb_t on_done_;
    std::optional<TokenizerError> error_; // set when skip extends past a buffer boundary
    bool skip_current_expression_ = false;
};


