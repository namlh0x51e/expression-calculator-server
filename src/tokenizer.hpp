#pragma once

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <variant>

constexpr auto FRAME_DELIM = '\x0A';

struct TokenNumber {
    int64_t value_;
};

struct TokenOperator {
    char op_;
};

struct TokenEof {};

using Token = std::variant<TokenNumber, TokenOperator, TokenEof>;

struct UnknownTokenError {
    std::size_t offset_;
    bool delimiter_consumed;
};

struct PartialTokenError {
    std::size_t offset_;
};

using TokenizerError = std::variant<PartialTokenError, UnknownTokenError>;

class Tokenizer {
   public:
    Tokenizer(std::span<std::byte> buffer) noexcept;

    auto next() noexcept -> bool;
    auto get() const noexcept -> Token const &;
    auto get_error() const noexcept -> std::optional<TokenizerError> const &;

   private:
    auto parse_next_token() noexcept -> std::expected<Token, TokenizerError>;

   private:
    std::span<std::byte> buffer_;
    std::size_t idx_{};
    Token next_token_;
    std::optional<TokenizerError> error_;
};
