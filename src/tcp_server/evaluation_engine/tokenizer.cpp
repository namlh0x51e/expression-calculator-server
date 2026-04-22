#include "tokenizer.hpp"

#include <algorithm>
#include <charconv>
#include <utility>

Tokenizer::Tokenizer(std::span<std::byte> buffer) noexcept : buffer_{std::move(buffer)} {}

auto Tokenizer::next() noexcept -> bool
{
    if (idx_ >= buffer_.size()) {
        return false;
    }

    auto token_result = parse_next_token();
    if (!token_result) {
        error_ = std::move(token_result.error());
        assert(error_.has_value());

        if (std::holds_alternative<PartialTokenError>(*error_)) {
            assert(idx_ == buffer_.size());

            auto const partial_token_offset = std::get<PartialTokenError>(*error_).offset_;

            assert(partial_token_offset < buffer_.size());

            std::memmove(buffer_.data(), buffer_.data() + partial_token_offset, buffer_.size() - partial_token_offset);
        }

        return false;
    }

    next_token_ = std::move(*token_result);

    return true;
}

auto Tokenizer::get() const noexcept -> Token const & { return next_token_; }

auto Tokenizer::get_error() const noexcept -> std::optional<TokenizerError> const & { return error_; }

auto Tokenizer::parse_next_token() noexcept -> std::expected<Token, TokenizerError>
{
    assert(buffer_.size() > 0);
    assert(idx_ < buffer_.size());

    auto is_operator = [](char c) noexcept {
        return c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')';
    };

    while (idx_ < buffer_.size()) {
        char const c = static_cast<char>(buffer_[idx_]);

        if (c == FRAME_DELIM) {
            ++idx_;
            return TokenEof{};
        }

        if (std::isdigit(c)) {
            auto const token_end_ptr =
                std::find_if_not(buffer_.data() + idx_, buffer_.data() + buffer_.size(),
                                 [](auto const b) noexcept { return std::isdigit(static_cast<char>(b)); });

            if (token_end_ptr == buffer_.data() + buffer_.size()) {
                return std::unexpected(PartialTokenError{std::exchange(idx_, buffer_.size())});
            }

            int64_t value;
            std::from_chars(reinterpret_cast<char *>(buffer_.data() + idx_), reinterpret_cast<char *>(token_end_ptr),
                            value);

            idx_ = static_cast<std::size_t>(std::distance(buffer_.data(), token_end_ptr));
            return TokenNumber{value};
        }

        if (is_operator(c)) {
            ++idx_;
            return TokenOperator{c};
        }

        if (std::isspace(c)) {
            ++idx_;
            continue;
        }

        // Unknown character — skip to next frame delimiter so the caller can
        // recover and process the next expression.
        auto const error_offset = idx_;
        while (idx_ < buffer_.size() && static_cast<char>(buffer_[idx_]) != FRAME_DELIM) {
            ++idx_;
        }
        bool const delimiter_consumed = idx_ < buffer_.size();
        if (delimiter_consumed) {
            ++idx_;
        }
        return std::unexpected(UnknownTokenError{error_offset, delimiter_consumed});
    }

    return std::unexpected(UnknownTokenError{idx_, false});
}
