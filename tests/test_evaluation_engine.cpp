#include <catch2/catch_test_macros.hpp>

#include "evaluation_engine.hpp"

#include <cstring>
#include <optional>
#include <string_view>
#include <vector>
#include <span>

namespace {

auto make_buffer(std::string_view sv) -> std::vector<std::byte>
{
    std::vector<std::byte> buf(sv.size());
    std::memcpy(buf.data(), sv.data(), sv.size());
    return buf;
}

auto eval_once(std::string_view expr) -> std::optional<std::expected<int64_t, EvalError>>
{
    EvaluationEngine engine;
    std::optional<std::expected<int64_t, EvalError>> result;
    engine.on_done([&result](auto r) { result = r; });
    auto buf = make_buffer(expr);
    engine.eval(std::span{buf});
    return result;
}

} // namespace

TEST_CASE("EvaluationEngine: simple addition", "[engine]")
{
    auto r = eval_once("3+2\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 5);
}

TEST_CASE("EvaluationEngine: simple subtraction", "[engine]")
{
    auto r = eval_once("10-4\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 6);
}

TEST_CASE("EvaluationEngine: simple multiplication", "[engine]")
{
    auto r = eval_once("3*7\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 21);
}

TEST_CASE("EvaluationEngine: simple division", "[engine]")
{
    auto r = eval_once("20/4\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 5);
}

TEST_CASE("EvaluationEngine: operator precedence mul over add", "[engine]")
{
    // 3+2*4 = 3+(2*4) = 11, not (3+2)*4 = 20
    auto r = eval_once("3+2*4\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 11);
}

TEST_CASE("EvaluationEngine: full expression from spec", "[engine]")
{
    // (3+2*4)*7 = (3+8)*7 = 77
    auto r = eval_once("(3+2*4)*7\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 77);
}

TEST_CASE("EvaluationEngine: nested parentheses", "[engine]")
{
    // ((2+3)*2)+1 = 11
    auto r = eval_once("((2+3)*2)+1\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 11);
}

TEST_CASE("EvaluationEngine: negative result", "[engine]")
{
    auto r = eval_once("3-10\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == -7);
}

TEST_CASE("EvaluationEngine: single number", "[engine]")
{
    auto r = eval_once("42\n");
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK(**r == 42);
}

TEST_CASE("EvaluationEngine: division by zero", "[engine]")
{
    auto r = eval_once("1/0\n");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
    CHECK(std::holds_alternative<EvalErrorDividedByZero>(r->error()));
}

TEST_CASE("EvaluationEngine: division by zero inside parens", "[engine]")
{
    auto r = eval_once("(1/0)\n");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
    CHECK(std::holds_alternative<EvalErrorDividedByZero>(r->error()));
}

TEST_CASE("EvaluationEngine: unmatched closing paren", "[engine]")
{
    auto r = eval_once("1)\n");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
    CHECK(std::holds_alternative<EvalErrorInvalidSyntax>(r->error()));
}

TEST_CASE("EvaluationEngine: trailing operator", "[engine]")
{
    auto r = eval_once("1+\n");
    REQUIRE(r.has_value());
    REQUIRE_FALSE(r->has_value());
    CHECK(std::holds_alternative<EvalErrorInsufficientOperandsAmount>(r->error()));
}

TEST_CASE("EvaluationEngine: error recovery — valid after error", "[engine]")
{
    EvaluationEngine engine;
    int call_count = 0;
    std::optional<std::expected<int64_t, EvalError>> last_result;
    engine.on_done([&](auto r) { ++call_count; last_result = r; });

    auto buf = make_buffer("1/0\n3+4\n");
    engine.eval(std::span{buf});

    CHECK(call_count == 2);
    REQUIRE(last_result.has_value());
    REQUIRE(last_result->has_value());
    CHECK(**last_result == 7);
}

TEST_CASE("EvaluationEngine: unknown token spanning buffer boundary", "[engine]")
{
    EvaluationEngine engine;
    int call_count = 0;
    std::optional<std::expected<int64_t, EvalError>> last_result;
    engine.on_done([&](auto r) {
        ++call_count;
        last_result = r;
    });

    // First buffer: unknown token, no delimiter — engine enters skip mode
    auto buf1 = make_buffer("@garbage");
    engine.eval(std::span{buf1});
    CHECK(call_count == 0); // no result yet, delimiter not seen

    // Second buffer: delimiter arrives mid-stream, followed by a valid expression
    auto buf2 = make_buffer("\n7*6\n");
    engine.eval(std::span{buf2});

    // on_done fired once for the garbage expression (EvalErrorInvalidSyntax)
    // and once for 7*6 = 42; last_result holds the second call
    CHECK(call_count == 2);
    REQUIRE(last_result.has_value());
    REQUIRE(last_result->has_value());
    CHECK(**last_result == 42);
}

TEST_CASE("EvaluationEngine: expression split — number at buffer boundary", "[engine][multi-buf]")
{
    EvaluationEngine engine;
    std::optional<std::expected<int64_t, EvalError>> result;
    engine.on_done([&](auto r) { result = r; });

    // Source expression "3*44\n" split after 3 bytes → buf1 = "3*4".
    auto buf1 = make_buffer("3*4");
    auto ret = engine.eval(std::span{buf1});
    CHECK(ret == 2);
    CHECK(!result.has_value());

    // buf1[0] now holds the partial byte '4' (memmoved from offset 2).
    // Construct buf2 = partial_bytes(buf1) + next_source_data("4\n") = "44\n".
    std::size_t partial_len = buf1.size() - ret;  // = 1
    auto buf2 = std::vector<std::byte>(buf1.begin(), buf1.begin() + static_cast<std::ptrdiff_t>(partial_len));
    auto next_source = make_buffer("4\n");
    buf2.insert(buf2.end(), next_source.begin(), next_source.end());

    (void)engine.eval(std::span{buf2});

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == 132);  // 3 * 44 = 132
}

TEST_CASE("EvaluationEngine: complete expression then partial number spans boundary", "[engine][multi-buf]")
{
    EvaluationEngine engine;
    int call_count = 0;
    std::vector<std::expected<int64_t, EvalError>> results;
    engine.on_done([&](auto r) { ++call_count; results.push_back(r); });

    auto buf1 = make_buffer("5+3\n10");
    auto ret = engine.eval(std::span{buf1});
    CHECK(ret == 4);
    CHECK(call_count == 1);
    REQUIRE(results[0].has_value());
    CHECK(results[0].value() == 8);

    auto buf2 = make_buffer("10+2\n");
    (void)engine.eval(std::span{buf2});

    CHECK(call_count == 2);
    REQUIRE(results[1].has_value());
    CHECK(results[1].value() == 12);
}

TEST_CASE("EvaluationEngine: skip_current_expression_ spans buffer boundary", "[engine][multi-buf]")
{
    EvaluationEngine engine;
    int call_count = 0;
    std::vector<std::expected<int64_t, EvalError>> results;
    engine.on_done([&](auto r) { ++call_count; results.push_back(r); });

    auto buf1 = make_buffer("1/0+5");
    auto ret = engine.eval(std::span{buf1});
    CHECK(ret == 4);
    CHECK(call_count == 1);
    REQUIRE_FALSE(results[0].has_value());
    CHECK(std::holds_alternative<EvalErrorDividedByZero>(results[0].error()));

    auto buf2 = make_buffer("5\n7+2\n");
    (void)engine.eval(std::span{buf2});

    CHECK(call_count == 2);
    REQUIRE(results[1].has_value());
    CHECK(results[1].value() == 9);
}

TEST_CASE("EvaluationEngine: expression split across three buffers", "[engine][multi-buf]")
{
    EvaluationEngine engine;
    std::optional<std::expected<int64_t, EvalError>> result;
    engine.on_done([&](auto r) { result = r; });

    auto buf1 = make_buffer("1+22");
    auto ret1 = engine.eval(std::span{buf1});
    CHECK(ret1 == 2);
    CHECK(!result.has_value());

    auto buf2 = make_buffer("22*3");
    auto ret2 = engine.eval(std::span{buf2});
    CHECK(ret2 == 3);
    CHECK(!result.has_value());

    auto buf3 = make_buffer("3\n");
    (void)engine.eval(std::span{buf3});

    REQUIRE(result.has_value());
    REQUIRE(result->has_value());
    CHECK(**result == 67);
}
