#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <asio.hpp>
#include <spdlog/spdlog.h>

using asio::ip::tcp;

// ── Interval arithmetic ───────────────────────────────────────────────────────
// Uses __int128 so range_mul of two in-range int64 values never overflows:
// INT64_MAX^2 ≈ 8.5×10^37 < INT128_MAX ≈ 1.7×10^38.

struct Range {
    __int128 lo, hi;
};

static constexpr __int128 I64_MAX_128 = INT64_MAX;
static constexpr __int128 I64_MIN_128 = INT64_MIN;

static bool in_i64(Range r) noexcept { return r.lo >= I64_MIN_128 && r.hi <= I64_MAX_128; }

static Range range_add(Range a, Range b) noexcept { return {a.lo + b.lo, a.hi + b.hi}; }
static Range range_sub(Range a, Range b) noexcept { return {a.lo - b.hi, a.hi - b.lo}; }

static Range range_mul(Range a, Range b) noexcept
{
    __int128 p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    return {*std::min_element(p, p + 4), *std::max_element(p, p + 4)};
}

static Range range_div(Range a, Range b) noexcept
{
    assert(b.lo >= 1);
    return {a.lo / b.hi, a.hi / b.lo};
}

// ── OutputBuffer (stdout mode only) ──────────────────────────────────────────

class OutputBuffer {
    static constexpr std::size_t CAPACITY = 4 * 1024 * 1024;

    alignas(64) char buf_[CAPACITY];
    std::size_t pos_ = 0;
    std::size_t written_ = 0;

   public:
    void put(char c)
    {
        buf_[pos_++] = c;
        if (pos_ == CAPACITY) flush();
    }

    void put(std::string_view sv)
    {
        for (char c : sv) put(c);
    }

    void flush()
    {
        if (pos_ == 0) return;
        std::size_t off = 0;
        while (off < pos_) {
            auto n = write(STDOUT_FILENO, buf_ + off, pos_ - off);
            if (n <= 0) std::exit(0);
            off += static_cast<std::size_t>(n);
        }
        written_ += pos_;
        pos_ = 0;
    }

    [[nodiscard]] std::size_t bytes_written() const noexcept { return written_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return pos_; }
};

// ── ExpressionGenerator ───────────────────────────────────────────────────────

class ExpressionGenerator {
    std::mt19937_64 rng_;
    int max_depth_;
    std::uniform_real_distribution<double> unit_{0.0, 1.0};
    std::uniform_int_distribution<int64_t> small_{1, 999};
    std::uniform_int_distribution<int64_t> large_{1000, 999999};

    bool coin(double p) { return unit_(rng_) < p; }

    Range gen_number(std::string& acc)
    {
        int64_t n = coin(0.7) ? small_(rng_) : large_(rng_);
        char tmp[20];
        auto res = std::to_chars(tmp, tmp + sizeof(tmp), n);
        acc.append(tmp, res.ptr);
        return {n, n};
    }

    Range gen_factor(int d, std::string& acc)
    {
        double const p = (d >= 2) ? 0.65 : (d == 1 ? 0.25 : 0.0);
        if (d > 0 && coin(p)) {
            acc += '(';
            Range r = gen_expr(d - 1, acc);
            acc += ')';
            return r;
        }
        return gen_number(acc);
    }

    Range gen_term(int d, std::string& acc)
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

    Range gen_expr(int d, std::string& acc)
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

    void gen_expression(std::string& out)
    {
        gen_expr(max_depth_, out);
        out += '\n';
    }
};

// ── Config ────────────────────────────────────────────────────────────────────

struct Config {
    int64_t     size_limit  = 1024LL * 1024 * 1024;
    int         max_depth   = 6;
    uint64_t    seed        = 42;
    std::string host;
    std::string port;
    int         connections = 1;

    [[nodiscard]] bool tcp_mode() const noexcept { return !host.empty() && !port.empty(); }
};

void usage(const char* prog)
{
    dprintf(STDERR_FILENO,
            "Usage: %s [options]\n\n"
            "  --size N[K|M|G]  Target output size (default: 1G).\n"
            "  --max-depth N    Max parenthesisation depth (default: 6, range: 1-64).\n"
            "  --seed N         PRNG seed (default: 42).\n"
            "  --host HOST      Server hostname/IP (enables TCP client mode).\n"
            "  --port PORT      Server port (required with --host).\n"
            "  --connections N  Concurrent TCP connections in TCP mode (default: 1).\n"
            "  --help           Print this message and exit.\n",
            prog);
}

int64_t parse_size(const char* s)
{
    char* end;
    int64_t n = std::strtoll(s, &end, 10);
    if (n <= 0) {
        dprintf(STDERR_FILENO, "error: --size must be a positive integer\n");
        std::exit(1);
    }
    if (*end == 'K' || *end == 'k')
        n *= 1024LL;
    else if (*end == 'M' || *end == 'm')
        n *= 1024LL * 1024;
    else if (*end == 'G' || *end == 'g')
        n *= 1024LL * 1024 * 1024;
    else if (*end != '\0') {
        dprintf(STDERR_FILENO, "error: unrecognised size suffix '%c'\n", *end);
        std::exit(1);
    }
    return n;
}

Config parse_args(int argc, char** argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        }
        auto need_next = [&](std::string_view flag) -> const char* {
            if (i + 1 >= argc) {
                dprintf(STDERR_FILENO, "error: %.*s requires a value\n",
                        static_cast<int>(flag.size()), flag.data());
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
        } else {
            dprintf(STDERR_FILENO, "error: unknown option '%.*s'\n",
                    static_cast<int>(arg.size()), arg.data());
            usage(argv[0]);
            std::exit(1);
        }
    }
    return cfg;
}

// ── TCP client ────────────────────────────────────────────────────────────────

struct ConnectionStats {
    uint64_t    exprs_sent     = 0;
    uint64_t    bytes_sent     = 0;
    uint64_t    responses_read = 0;
    bool        error          = false;
    std::string error_message;
};

asio::awaitable<void> drain_reader(tcp::socket& socket, ConnectionStats& stats)
{
    std::array<char, 64 * 1024> buf;
    while (true) {
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) break;
        for (std::size_t i = 0; i < n; ++i)
            if (buf[i] == '\n') ++stats.responses_read;
    }
}

asio::awaitable<void> run_connection(const Config& cfg, int conn_id, ConnectionStats& stats)
{
    auto exec = co_await asio::this_coro::executor;

    tcp::resolver resolver{exec};
    auto [rec, endpoints] = co_await resolver.async_resolve(
        cfg.host, cfg.port, asio::as_tuple(asio::use_awaitable));
    if (rec) {
        stats.error = true;
        stats.error_message = rec.message();
        co_return;
    }

    tcp::socket socket{exec};
    auto [ec, ep] = co_await asio::async_connect(
        socket, endpoints, asio::as_tuple(asio::use_awaitable));
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
        while (buf.size() < FLUSH &&
               stats.bytes_sent + buf.size() < static_cast<uint64_t>(cfg.size_limit)) {
            gen.gen_expression(buf);
            ++stats.exprs_sent;
        }
        auto [wec, n] = co_await asio::async_write(
            socket, asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
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

void print_summary(const std::vector<ConnectionStats>& stats, double elapsed_sec)
{
    uint64_t total_exprs     = 0;
    uint64_t total_bytes     = 0;
    uint64_t total_responses = 0;
    int      errors          = 0;

    for (const auto& s : stats) {
        total_exprs     += s.exprs_sent;
        total_bytes     += s.bytes_sent;
        total_responses += s.responses_read;
        if (s.error) {
            ++errors;
            dprintf(STDERR_FILENO, "  connection error: %s\n", s.error_message.c_str());
        }
    }

    double mb_sent       = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    double exprs_per_sec = static_cast<double>(total_exprs) / elapsed_sec;
    double mb_per_sec    = mb_sent / elapsed_sec;

    dprintf(STDERR_FILENO,
            "--- TCP client summary ---\n"
            "  connections   : %d\n"
            "  elapsed       : %.3f s\n"
            "  exprs sent    : %lu\n"
            "  bytes sent    : %lu (%.2f MB)\n"
            "  responses rcvd: %lu\n"
            "  throughput    : %.0f exprs/s, %.2f MB/s\n"
            "  errors        : %d\n",
            static_cast<int>(stats.size()),
            elapsed_sec,
            total_exprs,
            total_bytes, mb_sent,
            total_responses,
            exprs_per_sec, mb_per_sec,
            errors);
}

void run_tcp_client(const Config& cfg)
{
    asio::io_context ioc;
    std::vector<ConnectionStats> stats(static_cast<std::size_t>(cfg.connections));

    for (int id = 0; id < cfg.connections; ++id) {
        asio::co_spawn(ioc,
            run_connection(cfg, id, stats[static_cast<std::size_t>(id)]),
            asio::detached);
    }

    auto t_start = std::chrono::steady_clock::now();
    ioc.run();
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();

    print_summary(stats, elapsed);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);

    auto cfg = parse_args(argc, argv);

    if (cfg.host.empty() != cfg.port.empty()) {
        dprintf(STDERR_FILENO, "error: --host and --port must be used together\n");
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
