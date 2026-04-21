#include <spdlog/cfg/env.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <asio.hpp>
#include <cctype>
#include <charconv>
#include <cstring>
#include <exception>
#include <expected>
#include <iterator>
#include <list>
#include <memory>
#include <ranges>
#include <source_location>
#include <span>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

#include "asio/detached.hpp"

using asio::ip::tcp;
using thread_handle_t = uint32_t;

using namespace std::string_view_literals;

static_assert(sizeof(std::byte) == sizeof(unsigned char));

constexpr auto BUFFER_POOL_SIZE = 1024;
constexpr auto BUFFER_SIZE = 64 * 1024;
constexpr auto FRAME_DELIM = '\x0A';

namespace logger = spdlog;

auto get_thread_id() noexcept -> std::string
{
    auto tid = std::this_thread::get_id();
    std::stringstream ss;
    ss << tid;
    return ss.str();
}

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

    while(idx_ < buffer_.size()) {
        char const c = static_cast<char>(buffer_[idx_]);

        if (c == FRAME_DELIM) {
            ++idx_;
            return TokenEof{};
        }

        if (std::isdigit(c)) {
            auto const token_end_ptr =
                std::find_if_not(buffer_.data() + idx_, buffer_.data() + buffer_.size(),
                                 [](auto const c) noexcept { return std::isdigit(static_cast<char>(c)); });

            if (token_end_ptr == buffer_.data() + buffer_.size()) {
                return std::unexpected(PartialTokenError{std::exchange(idx_, buffer_.size())});
            }

            int64_t value;
            std::from_chars(reinterpret_cast<char *>(buffer_.data() + idx_), reinterpret_cast<char *>(token_end_ptr),
                            value);

            idx_ = std::distance(buffer_.data(), token_end_ptr);
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
    };

    return std::unexpected(UnknownTokenError{idx_});
}

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
};

auto EvaluationEngine::eval(std::span<std::byte> buffer) noexcept -> std::size_t
{
    Tokenizer tokenizer{buffer};

    while (tokenizer.next()) {
        auto const &token = tokenizer.get();
        if (std::holds_alternative<TokenNumber>(token)) {
            logger::debug("TokenNumber({})", std::get<TokenNumber>(token).value_);
            value_stack_.emplace(std::get<TokenNumber>(token).value_);
        } else if (std::holds_alternative<TokenOperator>(token)) {
            auto const op = std::get<TokenOperator>(token).op_;

            logger::debug("TokenOperator({})", op);

            switch (op) {
                case '(':
                    op_stack_.push(op);
                    break;
                case ')':
                    while (!op_stack_.empty() && op_stack_.top() != '(') {
                        auto result = pop_operator_and_eval();
                        if (!result) {
                            // TODO
                            auto src_loc = std::source_location::current();
                            logger::debug("Unimplemented: {}", src_loc.line());
                            throw std::runtime_error("Unimplented");
                        }
                    }

                    if (op_stack_.empty()) {
                        // TODO
                        auto src_loc = std::source_location::current();
                        logger::debug("Unimplemented: {}", src_loc.line());
                        throw std::runtime_error("Unimplented");
                    }

                    op_stack_.pop();
                    break;
                default:
                    while (!op_stack_.empty() && op_stack_.top() != '(' &&
                           get_precedence(op_stack_.top()) >= get_precedence(op)) {
                        auto result = pop_operator_and_eval();
                        if (!result) {
                            // TODO
                            auto src_loc = std::source_location::current();
                            logger::debug("Unimplemented: {}", src_loc.line());
                            throw std::runtime_error("Unimplented");
                        }
                    }

                    op_stack_.push(op);
                    break;
            }
        } else if (std::holds_alternative<TokenEof>(token)) {
            logger::debug("TokenEof()");

            while (!op_stack_.empty()) {
                auto result = pop_operator_and_eval();
                if (!result) {
                    // TODO
                    auto src_loc = std::source_location::current();
                    logger::debug("Unimplemented: {}", src_loc.line());
                    throw std::runtime_error("Unimplented");
                }
            }

            if (value_stack_.empty()) {
                continue;
            }

            if (value_stack_.size() != 1) {
                while (!value_stack_.empty()) value_stack_.pop();
                while (!op_stack_.empty()) op_stack_.pop();
                if (on_done_) {
                    on_done_(std::unexpected(EvalErrorInvalidSyntax{}));
                }
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
            // Unknown token error
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

class Connection {
   public:
    using on_close_cb_t = std::function<void()>;

    Connection(tcp::socket socket) noexcept;

    auto on_close(on_close_cb_t) noexcept -> void;

   private:
    auto serve() -> asio::awaitable<void>;

   private:
    tcp::socket socket_;
    EvaluationEngine engine_;
    std::array<std::byte, BUFFER_SIZE> buffer_;

    on_close_cb_t on_close_;
};

Connection::Connection(tcp::socket socket) noexcept : socket_{std::move(socket)}
{
    engine_.on_done([this](auto result) {
        asio::post(socket_.get_executor(), [this, result]() {
            // int64_t is at most 20 characters long
            std::array<std::byte, 24> reply_frame{};
            std::size_t n;

            if (result) {
                logger::debug("Write back result value: {}", *result);
                n = std::snprintf(reinterpret_cast<char *>(reply_frame.data()), reply_frame.size(), "%ld\n", *result);
            } else {
                logger::debug("Write back result value: \"SYNTAX ERROR\"");
                n = std::snprintf(reinterpret_cast<char *>(reply_frame.data()), reply_frame.size(), "SYNTAX ERROR\n");
            }

            asio::async_write(socket_, asio::buffer(reply_frame.data(), n), [](std::error_code ec, auto) {
                if (ec) {
                    logger::error("Write back result failed: ", ec.message());
                }
            });
        });
    });

    asio::co_spawn(socket.get_executor(), [this]() { return serve(); }, asio::detached);
}

auto Connection::on_close(on_close_cb_t cb) noexcept -> void { on_close_ = cb; }

auto Connection::serve() -> asio::awaitable<void>
{
    try {
        std::size_t offset = 0;
        while (true) {
            auto const n_bytes =
                co_await socket_.async_read_some(asio::buffer(buffer_.data() + offset, buffer_.size() - offset));
            logger::debug("Byte stream received: {}",
                          spdlog::to_hex(buffer_.data() + offset, buffer_.data() + offset + n_bytes));
            offset = engine_.eval({buffer_.data(), n_bytes});
        }
    } catch (std::exception const &e) {
        logger::error("Exception: ", e.what());
    }
}

class TcpServer {
   public:
    using executor_type = asio::io_context::executor_type;

    TcpServer() noexcept = default;
    ~TcpServer();
    auto stop() noexcept -> void;
    auto listen_and_serve(asio::io_context &ctx, std::string_view address, uint16_t port) noexcept -> void;

   private:
    std::list<std::unique_ptr<Connection>> connections_;
    bool is_stopped_{false};
};

class WorkerThreadManager {
   public:
    using task_t = std::function<void()>;

    WorkerThreadManager(int32_t num_threads = std::max(std::thread::hardware_concurrency(), 1U)) noexcept;

    ~WorkerThreadManager();

    auto serve(std::string_view address, uint16_t port) noexcept -> void;
    auto stop() noexcept -> void;

   private:
    using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;

    struct WorkerThread {
        asio::io_context ctx_;
        TcpServer tcp_server_;
        std::thread thread_;
        work_guard_t work_guard_;

        WorkerThread() noexcept;

        auto serve(std::string_view address, uint16_t port) noexcept -> void;
        auto join() noexcept -> void;
    };

    std::vector<std::unique_ptr<WorkerThread>> worker_threads_;

    bool is_stopped_{false};
};

class SignalListener {
   public:
    SignalListener(WorkerThreadManager &worker_thread_manager) noexcept;

    auto run() noexcept -> void;

   private:
    auto handle_signal() -> asio::awaitable<void>;

   private:
    WorkerThreadManager &worker_thread_manager_;
    asio::io_context ctx_;
};

SignalListener::SignalListener(WorkerThreadManager &worker_thread_manager) noexcept
    : worker_thread_manager_{worker_thread_manager}
{
}

auto SignalListener::run() noexcept -> void
{
    asio::co_spawn(ctx_, [this]() noexcept { return handle_signal(); }, asio::detached);

    // TODO: remove this and implement signal handler logic
    auto work = asio::make_work_guard(ctx_);

    ctx_.run();
}

auto SignalListener::handle_signal() -> asio::awaitable<void>
{
    // TODO: handle signal handler logic
    co_return;
}

WorkerThreadManager::WorkerThread::WorkerThread() noexcept : tcp_server_{}, work_guard_{asio::make_work_guard(ctx_)} {}

auto WorkerThreadManager::WorkerThread::serve(std::string_view address, uint16_t port) noexcept -> void
{
    thread_ = std::thread{[this, address, port]() {
        auto tid = get_thread_id();
        logger::debug("Thread {} start", tid);
        tcp_server_.listen_and_serve(ctx_, address, port);
        logger::debug("Thread {} done", tid);
    }};
}

auto WorkerThreadManager::WorkerThread::join() noexcept -> void
{
    work_guard_.reset();
    thread_.join();
}

WorkerThreadManager::WorkerThreadManager(int32_t num_threads) noexcept
{
    worker_threads_.reserve(num_threads);

    for (auto const _ : std::views::iota(0, num_threads)) {
        worker_threads_.emplace_back(std::make_unique<WorkerThreadManager::WorkerThread>());
    }
}

WorkerThreadManager::~WorkerThreadManager() { stop(); }

auto WorkerThreadManager::serve(std::string_view address, uint16_t port) noexcept -> void
{
    for (auto &worker_thread : worker_threads_) {
        worker_thread->serve(address, port);
    }

    logger::info("Tcp server listen and serve at {}:{}", address, port);
}

auto WorkerThreadManager::stop() noexcept -> void
{
    if (std::exchange(is_stopped_, true)) {
        return;
    }

    for (auto &worker_thread : worker_threads_) {
        worker_thread->join();
    }
}

TcpServer::~TcpServer() { stop(); }

auto TcpServer::stop() noexcept -> void
{
    if (std::exchange(is_stopped_, true)) {
        return;
    }
}

auto TcpServer::listen_and_serve(asio::io_context &ctx, std::string_view address, uint16_t port) noexcept -> void
{
    asio::co_spawn(
        ctx,
        [this, exec = ctx.get_executor(), address, port]() -> asio::awaitable<void> {
            tcp::acceptor acceptor{exec};
            tcp::endpoint endpoint{asio::ip::make_address(address), port};

            acceptor.open(endpoint.protocol());

            // Layer 4 load balancing connections to threads
            int optval = 1;
            ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

            acceptor.set_option(tcp::acceptor::reuse_address(true));
            acceptor.bind(endpoint);
            acceptor.listen();

            while (true) {
                try {
                    tcp::socket socket{exec};

                    co_await acceptor.async_accept(socket);

                    socket.set_option(tcp::no_delay(true));

                    auto &conn = connections_.emplace_front(std::make_unique<Connection>(std::move(socket)));

                    conn->on_close([this, it = connections_.begin()]() noexcept { connections_.erase(it); });

                    logger::debug("Thread id = {} accept connection", get_thread_id());

                } catch (const std::exception &e) {
                    logger::error("Exception while listening for connections: {}", e.what());
                }
            }
        },
        asio::detached);

    ctx.run();
}

int main(int argc, char **argv)
{
    spdlog::cfg::load_env_levels();

    uint16_t const port = (argc == 2) ? std::stoi(argv[1]) : 8000;

    WorkerThreadManager worker_thread_manager{};

    worker_thread_manager.serve("0.0.0.0", port);

    SignalListener signal_listener{worker_thread_manager};
    signal_listener.run();
}
