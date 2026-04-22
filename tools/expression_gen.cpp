#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <cassert>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using asio::ip::tcp;

struct Range {
    // Uses __int128 so range_mul of two in-range int64 values never overflows:
    __int128 lo, hi;
};

constexpr __int128 I64_MAX_128 = INT64_MAX;
constexpr __int128 I64_MIN_128 = INT64_MIN;

bool in_i64(Range r) noexcept { return r.lo >= I64_MIN_128 && r.hi <= I64_MAX_128; }

Range range_add(Range a, Range b) noexcept { return {a.lo + b.lo, a.hi + b.hi}; }
Range range_sub(Range a, Range b) noexcept { return {a.lo - b.hi, a.hi - b.lo}; }

Range range_mul(Range a, Range b) noexcept
{
    __int128 p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
}

Range range_div(Range a, Range b) noexcept
{
    assert(b.lo >= 1);
    return {a.lo / b.hi, a.hi / b.lo};
}

class OutputBuffer {
    static constexpr std::size_t CAPACITY = 4 * 1024 * 1024;

    char buf_[CAPACITY];
    std::size_t pos_ = 0;
    std::size_t written_ = 0;

   public:
    auto put(char c) noexcept -> void
    {
        buf_[pos_++] = c;
        if (pos_ == CAPACITY) flush();
    }

    void put(std::string_view sv)
    {
        for (char c : sv) put(c);
    }

    auto flush() noexcept -> void
    {
        if (pos_ == 0) return;
        std::size_t off = 0;
        while (off < pos_) {
            auto n = ::write(STDOUT_FILENO, buf_ + off, pos_ - off);
            if (n <= 0) std::exit(0);
            off += static_cast<std::size_t>(n);
        }
        written_ += pos_;
        pos_ = 0;
    }

    [[nodiscard]] auto bytes_written() const noexcept -> std::size_t { return written_; }
    [[nodiscard]] auto buffered() const noexcept -> std::size_t { return pos_; }
};

class ExpressionGenerator {
    std::mt19937_64 rng_;
    int max_depth_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::uniform_int_distribution<int64_t> small_{1, 999};
    std::uniform_int_distribution<int64_t> large_{1000, 999999};

    auto coin(double p) noexcept -> bool { return unit_(rng_) < p; }

    auto gen_number(std::string& acc) noexcept -> Range
    {
        int64_t n = coin(0.7) ? small_(rng_) : large_(rng_);
        std::array<char, 20> tmp;
        auto res = std::to_chars(tmp.data(), tmp.data() + tmp.size(), n);
        acc.append(tmp.data(), res.ptr);
        return {n, n};
    }

    auto gen_factor(int d, std::string& acc) noexcept -> Range
    {
        double const p = (d >= 2) ? 0.65 : (d == 1 ? 0.25 : 0.0);
        if (d > 0 && coin(p)) {
            acc += '(';
            auto r = gen_expr(d - 1, acc);
            acc += ')';
            return r;
        }
        return gen_number(acc);
    }

    auto gen_term(int d, std::string& acc) noexcept -> Range
    {
        auto r_acc = gen_factor(d, acc);
        while (coin(0.5)) {
            char op = coin(0.5) ? '*' : '/';
            std::string tmp;
            Range r2 = gen_factor(d, tmp);
            if (op == '/' && r2.lo <= 0) op = '*';
            Range r_new = (op == '*') ? range_mul(r_acc, r2) : range_div(r_acc, r2);
            if (!in_i64(r_new)) break;
            acc += op;
            acc += tmp;
            r_acc = r_new;
        }
        return r_acc;
    }

    auto gen_expr(int d, std::string& acc) noexcept -> Range
    {
        auto r_acc = gen_term(d, acc);
        while (coin(0.5)) {
            char op = coin(0.5) ? '+' : '-';
            std::string tmp;
            auto r2 = gen_term(d, tmp);
            auto r_new = (op == '+') ? range_add(r_acc, r2) : range_sub(r_acc, r2);
            if (!in_i64(r_new)) break;
            acc += op;
            acc += tmp;
            r_acc = r_new;
        }
        return r_acc;
    }

   public:
    ExpressionGenerator(uint64_t seed, int max_depth) : rng_{seed}, max_depth_{max_depth} {}

    auto gen_expression(std::string& out) noexcept -> void
    {
        gen_expr(max_depth_, out);
        out += '\n';
    }
};

struct Config {
    int64_t size_limit = 1024LL * 1024 * 1024;
    int max_depth = 6;
    uint64_t seed = 42;
    std::string host;
    std::string port;
    int connections = 1;
    int threads = 1;

    [[nodiscard]] bool tcp_mode() const noexcept { return !host.empty() && !port.empty(); }
};

auto print_usage(const char* prog) noexcept -> void
{
    std::println(std::cerr,
                 "Usage: {} [options]\n\n"
                 "  --size N[K|M|G]  Target output size (default: 1G).\n"
                 "  --max-depth N    Max parenthesisation depth (default: 6, range: 1-64).\n"
                 "  --seed N         PRNG seed (default: 42).\n"
                 "  --host HOST      Server hostname/IP (enables TCP client mode).\n"
                 "  --port PORT      Server port (required with --host).\n"
                 "  --connections N  Concurrent TCP connections in TCP mode (default: 1).\n"
                 "  --threads N      I/O threads for TCP mode (default: 1).\n"
                 "  --help           Print this message and exit.",
                 prog);
}

auto parse_size(const char* s) noexcept -> int64_t
{
    char* end;
    int64_t n = std::strtoll(s, &end, 10);
    if (n <= 0) {
        std::println(std::cerr, "error: --size must be a positive integer");
        std::exit(1);
    }
    if (*end == 'K' || *end == 'k')
        n *= 1024LL;
    else if (*end == 'M' || *end == 'm')
        n *= 1024LL * 1024;
    else if (*end == 'G' || *end == 'g')
        n *= 1024LL * 1024 * 1024;
    else if (*end != '\0') {
        std::println(std::cerr, "error: unrecognised size suffix '{}'\n", *end);
        std::exit(1);
    }
    return n;
}

auto parse_args(int argc, char** argv) noexcept -> Config
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        }
        auto need_next = [&](std::string_view flag) -> const char* {
            if (i + 1 >= argc) {
                std::println(std::cerr, "error: {} requires a value\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--size") {
            cfg.size_limit = parse_size(need_next(arg));
        } else if (arg == "--max-depth") {
            cfg.max_depth = static_cast<int>(std::strtol(need_next(arg), nullptr, 10));
            if (cfg.max_depth < 1) cfg.max_depth = 1;
            if (cfg.max_depth > 64) cfg.max_depth = 64;
        } else if (arg == "--seed") {
            cfg.seed = std::strtoull(need_next(arg), nullptr, 10);
        } else if (arg == "--host") {
            cfg.host = need_next(arg);
        } else if (arg == "--port") {
            cfg.port = need_next(arg);
        } else if (arg == "--connections") {
            cfg.connections = static_cast<int>(std::strtol(need_next(arg), nullptr, 10));
            if (cfg.connections < 1) cfg.connections = 1;
        } else if (arg == "--threads") {
            cfg.threads = static_cast<int>(std::strtol(need_next(arg), nullptr, 10));
            if (cfg.threads < 1) cfg.threads = 1;
        } else {
            std::println(std::cerr, "error: unknown option '{}'", arg);
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

struct ConnectionStats {
    uint64_t exprs_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t responses_read = 0;
    bool error = false;
    std::string error_message;
};

auto drain_reader(tcp::socket& socket, ConnectionStats& stats) noexcept -> asio::awaitable<void>
{
    std::array<char, 64 * 1024> buf;
    while (true) {
        auto [ec, n] = co_await socket.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) break;
        for (std::size_t i = 0; i < n; ++i)
            if (buf[i] == '\n') ++stats.responses_read;
    }
}

auto run_connection(const Config& cfg, int conn_id, ConnectionStats& stats) noexcept -> asio::awaitable<void>
{
    auto exec = co_await asio::this_coro::executor;

    tcp::resolver resolver{exec};
    auto [rec, endpoints] = co_await resolver.async_resolve(cfg.host, cfg.port, asio::as_tuple(asio::use_awaitable));
    if (rec) {
        stats.error = true;
        stats.error_message = rec.message();
        co_return;
    }

    tcp::socket socket{exec};
    auto [ec, ep] = co_await asio::async_connect(socket, endpoints, asio::as_tuple(asio::use_awaitable));
    (void)ep;
    if (ec) {
        stats.error = true;
        stats.error_message = ec.message();
        co_return;
    }

    socket.set_option(tcp::no_delay(true));

    asio::co_spawn(exec, drain_reader(socket, stats), asio::detached);

    ExpressionGenerator gen{cfg.seed + static_cast<uint64_t>(conn_id), cfg.max_depth};
    std::string buf;
    buf.reserve(256 * 1024);
    constexpr std::size_t FLUSH = 64 * 1024;

    while (stats.bytes_sent < static_cast<uint64_t>(cfg.size_limit)) {
        buf.clear();
        while (buf.size() < FLUSH && stats.bytes_sent + buf.size() < static_cast<uint64_t>(cfg.size_limit)) {
            gen.gen_expression(buf);
            ++stats.exprs_sent;
        }
        auto [wec, n] = co_await asio::async_write(socket, asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (wec) {
            stats.error = true;
            stats.error_message = wec.message();
            co_return;
        }
        stats.bytes_sent += n;
    }

    std::error_code sec;
    socket.shutdown(tcp::socket::shutdown_send, sec);
}

auto print_summary(const std::vector<ConnectionStats>& stats, double elapsed_sec) noexcept -> void
{
    uint64_t total_exprs = 0;
    uint64_t total_bytes = 0;
    uint64_t total_responses = 0;
    int errors = 0;

    for (const auto& s : stats) {
        total_exprs += s.exprs_sent;
        total_bytes += s.bytes_sent;
        total_responses += s.responses_read;
        if (s.error) {
            ++errors;
            std::println(std::cerr, "  connection error: {}", s.error_message);
        }
    }

    double mb_sent = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double exprs_per_sec = static_cast<double>(total_exprs) / elapsed_sec;
    double mb_per_sec = mb_sent / elapsed_sec;

    std::println(std::cerr,
                 "--- TCP client summary ---\n"
                 "  connections   : {}\n"
                 "  elapsed       : {:.3f} s\n"
                 "  exprs sent    : {}\n"
                 "  bytes sent    : {} ({:.2f} MB)\n"
                 "  responses rcvd: {}\n"
                 "  throughput    : {:.0f} exprs/s, {:.2f} MB/s\n"
                 "  errors        : {}\n",
                 static_cast<int>(stats.size()), elapsed_sec, total_exprs, total_bytes, mb_sent, total_responses,
                 exprs_per_sec, mb_per_sec, errors);
}

auto run_tcp_client(const Config& cfg) -> void
{
    asio::io_context ioc;
    std::vector<ConnectionStats> stats(static_cast<std::size_t>(cfg.connections));

    for (int id = 0; id < cfg.connections; ++id) {
        asio::co_spawn(ioc, run_connection(cfg, id, stats[static_cast<std::size_t>(id)]), asio::detached);
    }

    auto t_start = std::chrono::steady_clock::now();

    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(cfg.threads - 1));
    for (int t = 1; t < cfg.threads; ++t)
        pool.emplace_back([&ioc] { ioc.run(); });
    ioc.run();
    for (auto& t : pool) t.join();

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

    print_summary(stats, elapsed);
}

auto main(int argc, char** argv) -> int
{
    signal(SIGPIPE, SIG_IGN);

    auto cfg = parse_args(argc, argv);

    if (cfg.host.empty() != cfg.port.empty()) {
        std::println(std::cerr, "error: --host and --port must be used together");
        return 1;
    }

    if (cfg.tcp_mode()) {
        run_tcp_client(cfg);
        return 0;
    }

    // Stdout mode
    OutputBuffer out;
    ExpressionGenerator gen{cfg.seed, cfg.max_depth};
    std::string tmp;
    auto limit = static_cast<std::size_t>(cfg.size_limit);

    while (out.bytes_written() + out.buffered() < limit) {
        tmp.clear();
        gen.gen_expression(tmp);
        out.put(std::string_view{tmp});
    }

    out.flush();
    return 0;
}
