#include "evaluation_engine.hpp"

auto EvaluationEngine::eval(std::span<std::byte> buffer) noexcept -> std::size_t
{
    if (error_.has_value()) {
        // Mid-skip from a previous call — scan for the frame delimiter.
        auto it =
            std::find_if(buffer.begin(), buffer.end(), [](std::byte b) { return static_cast<char>(b) == FRAME_DELIM; });
        if (it == buffer.end()) {
            return 0;  // delimiter still not in this buffer; keep skipping next call
        }
        error_.reset();  // frame delimiter met — exit skip mode
        // The full (invalid) expression has now been consumed; report the error.
        if (on_done_) {
            on_done_(std::unexpected(EvalErrorInvalidSyntax{}));
        }
        auto const delim_pos = static_cast<std::size_t>(it - buffer.begin());
        buffer = buffer.subspan(delim_pos + 1);
        if (buffer.empty()) return 0;
    }

    Tokenizer tokenizer{buffer};

    auto report_error_and_skip = [&](EvalError err) noexcept {
        while (!value_stack_.empty()) value_stack_.pop();
        while (!op_stack_.empty()) op_stack_.pop();
        if (on_done_) on_done_(std::unexpected(std::move(err)));
        skip_current_expression_ = true;
    };

    while (tokenizer.next()) {
        auto const& token = tokenizer.get();

        if (skip_current_expression_) {
            if (std::holds_alternative<TokenEof>(token)) {
                skip_current_expression_ = false;
            }
            continue;
        }

        if (std::holds_alternative<TokenNumber>(token)) {
            spdlog::debug("TokenNumber({})", std::get<TokenNumber>(token).value_);
            value_stack_.emplace(std::get<TokenNumber>(token).value_);
        } else if (std::holds_alternative<TokenOperator>(token)) {
            auto const op = std::get<TokenOperator>(token).op_;

            spdlog::debug("TokenOperator({})", op);

            switch (op) {
                case '(':
                    op_stack_.push(op);
                    break;
                case ')':
                    while (!op_stack_.empty() && op_stack_.top() != '(') {
                        auto result = pop_operator_and_eval();
                        if (!result) {
                            report_error_and_skip(std::move(result.error()));
                            break;
                        }
                    }
                    if (skip_current_expression_) break;

                    if (op_stack_.empty()) {
                        report_error_and_skip(EvalErrorInvalidSyntax{});
                        break;
                    }
                    op_stack_.pop();
                    break;
                default:
                    while (!op_stack_.empty() && op_stack_.top() != '(' &&
                           get_precedence(op_stack_.top()) >= get_precedence(op)) {
                        auto result = pop_operator_and_eval();
                        if (!result) {
                            report_error_and_skip(std::move(result.error()));
                            break;
                        }
                    }
                    if (skip_current_expression_) break;
                    op_stack_.push(op);
                    break;
            }
        } else if (std::holds_alternative<TokenEof>(token)) {
            spdlog::debug("TokenEof()");

            bool eof_error = false;
            while (!op_stack_.empty()) {
                auto result = pop_operator_and_eval();
                if (!result) {
                    while (!value_stack_.empty()) value_stack_.pop();
                    while (!op_stack_.empty()) op_stack_.pop();
                    if (on_done_) on_done_(std::unexpected(std::move(result.error())));
                    eof_error = true;
                    break;
                }
            }
            if (eof_error) continue;

            if (value_stack_.empty()) {
                continue;
            }

            if (value_stack_.size() != 1) {
                while (!value_stack_.empty()) value_stack_.pop();
                if (on_done_) on_done_(std::unexpected(EvalErrorInvalidSyntax{}));
                continue;
            }

            if (on_done_) {
                auto result = value_stack_.top();
                value_stack_.pop();
                on_done_(result);
            }

            assert(value_stack_.empty());
        }
    }

    if (auto err = tokenizer.get_error(); err) {
        if (std::holds_alternative<PartialTokenError>(*err)) {
            auto const unhandled_offset = std::get<PartialTokenError>(*err).offset_;
            return unhandled_offset;
        } else {
            auto const& unknown_err = std::get<UnknownTokenError>(*err);
            while (!value_stack_.empty()) value_stack_.pop();
            while (!op_stack_.empty()) op_stack_.pop();
            if (unknown_err.delimiter_consumed) {
                // Full invalid expression consumed in this buffer — report immediately.
                if (on_done_) {
                    on_done_(std::unexpected(EvalErrorInvalidSyntax{}));
                }
            } else {
                // Delimiter not yet seen — defer callback until next call finds it.
                error_ = *err;
            }
        }
    }

    return 0;
}

auto EvaluationEngine::on_done(on_done_cb_t cb) noexcept -> void { on_done_ = cb; }

constexpr auto EvaluationEngine::get_precedence(char op) noexcept -> uint16_t
{
    switch (op) {
        case '+':
            return 1;
        case '-':
            return 1;
        case '*':
            return 2;
        case '/':
            return 2;
        default:
            std::unreachable();
    }

    std::unreachable();
}

constexpr auto EvaluationEngine::apply(char op, int64_t v1, int64_t v2) noexcept
    -> std::expected<int64_t, EvalErrorDividedByZero>
{
    switch (op) {
        case '+':
            return v1 + v2;
        case '-':
            return v1 - v2;
        case '*':
            return v1 * v2;
        case '/':
            if (v2 == 0) {
                return std::unexpected(EvalErrorDividedByZero{});
            }
            return v1 / v2;
        default:
            std::unreachable();
    }

    std::unreachable();
}

auto EvaluationEngine::pop_operator_and_eval() noexcept -> std::expected<void, EvalError>
{
    assert(!op_stack_.empty());

    if (value_stack_.size() < 2) {
        return std::unexpected(EvalErrorInsufficientOperandsAmount{});
    }

    auto v2 = value_stack_.top();
    value_stack_.pop();

    auto v1 = value_stack_.top();
    value_stack_.pop();

    auto apply_result = apply(op_stack_.top(), v1, v2);
    if (!apply_result) {
        return std::unexpected(std::move(apply_result.error()));
    }

    value_stack_.emplace(*apply_result);
    op_stack_.pop();

    return {};
}
