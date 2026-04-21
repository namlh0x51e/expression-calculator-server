#include <catch2/catch_test_macros.hpp>

#include "tokenizer.hpp"

#include <cstring>
#include <string_view>
#include <vector>

namespace {

auto make_buffer(std::string_view sv) -> std::vector<std::byte>
{
    std::vector<std::byte> buf(sv.size());
    std::memcpy(buf.data(), sv.data(), sv.size());
    return buf;
}

} // namespace

TEST_CASE("Tokenizer: single integer", "[tokenizer]")
{
    auto buf = make_buffer("42\n");
    Tokenizer t{std::span{buf}};

    REQUIRE(t.next());
    CHECK(std::holds_alternative<TokenNumber>(t.get()));
    CHECK(std::get<TokenNumber>(t.get()).value_ == 42);

    REQUIRE(t.next());
    CHECK(std::holds_alternative<TokenEof>(t.get()));

    CHECK_FALSE(t.next());
}

TEST_CASE("Tokenizer: operators and parentheses", "[tokenizer]")
{
    auto buf = make_buffer("+-*/()\n");
    Tokenizer t{std::span{buf}};

    for (char expected : {'+', '-', '*', '/', '(', ')'}) {
        REQUIRE(t.next());
        REQUIRE(std::holds_alternative<TokenOperator>(t.get()));
        CHECK(std::get<TokenOperator>(t.get()).op_ == expected);
    }

    REQUIRE(t.next());
    CHECK(std::holds_alternative<TokenEof>(t.get()));
}

TEST_CASE("Tokenizer: whitespace is skipped", "[tokenizer]")
{
    auto buf = make_buffer("  3  +  4  \n");
    Tokenizer t{std::span{buf}};

    REQUIRE(t.next());
    CHECK(std::get<TokenNumber>(t.get()).value_ == 3);

    REQUIRE(t.next());
    CHECK(std::get<TokenOperator>(t.get()).op_ == '+');

    REQUIRE(t.next());
    CHECK(std::get<TokenNumber>(t.get()).value_ == 4);

    REQUIRE(t.next());
    CHECK(std::holds_alternative<TokenEof>(t.get()));
}

TEST_CASE("Tokenizer: partial token at buffer boundary", "[tokenizer]")
{
    // "12" without a newline — tokenizer cannot know if more digits follow
    auto buf = make_buffer("12");
    Tokenizer t{std::span{buf}};

    CHECK_FALSE(t.next());
    REQUIRE(t.get_error().has_value());
    CHECK(std::holds_alternative<PartialTokenError>(*t.get_error()));
}

TEST_CASE("Tokenizer: unknown token returns UnknownTokenError", "[tokenizer]")
{
    auto buf = make_buffer("@\n");
    Tokenizer t{std::span{buf}};

    CHECK_FALSE(t.next());
    REQUIRE(t.get_error().has_value());
    CHECK(std::holds_alternative<UnknownTokenError>(*t.get_error()));
}

TEST_CASE("Tokenizer: multi-digit integer", "[tokenizer]")
{
    auto buf = make_buffer("9876543210\n");
    Tokenizer t{std::span{buf}};

    REQUIRE(t.next());
    CHECK(std::get<TokenNumber>(t.get()).value_ == 9876543210LL);
}

TEST_CASE("Tokenizer: empty line yields immediate EOF", "[tokenizer]")
{
    auto buf = make_buffer("\n");
    Tokenizer t{std::span{buf}};

    REQUIRE(t.next());
    CHECK(std::holds_alternative<TokenEof>(t.get()));
}

TEST_CASE("Tokenizer: skips to frame delimiter on unknown token", "[tokenizer]")
{
    // Unknown char followed by more garbage and a delimiter — tokenizer must
    // consume everything including the '\n' so no bytes are left over.
    auto buf = make_buffer("@bad\n");
    Tokenizer t{std::span{buf}};

    CHECK_FALSE(t.next());
    REQUIRE(t.get_error().has_value());
    CHECK(std::holds_alternative<UnknownTokenError>(*t.get_error()));
    // Buffer fully consumed — subsequent call must return false without looping.
    CHECK_FALSE(t.next());
}

