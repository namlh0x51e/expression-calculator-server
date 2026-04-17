#include <algorithm>
#include <asio.hpp>
#include <cctype>
#include <charconv>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <list>
#include <memory>
#include <ranges>
#include <span>
#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

using asio::ip::tcp;
using thread_handle_t = uint32_t;

using namespace std::string_view_literals;

static_assert(sizeof(std::byte) == sizeof(unsigned char));

constexpr auto BUFFER_POOL_SIZE = 1024;
constexpr auto BUFFER_SIZE = 64 * 1024;
constexpr auto FRAME_DELIM = '\x0A';

constexpr auto SYNTAX_ERROR_FRAME = "Syntax error\r\n";

namespace logger = spdlog;

auto get_thread_id() noexcept -> std::string {
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

struct TokenEmpty {};

using Token = std::variant<TokenNumber, TokenOperator, TokenEof, TokenEmpty>;

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
  auto get() const noexcept -> std::variant<TokenNumber, TokenOperator> const &;
  auto get_error() const noexcept -> std::optional<TokenizerError> const &;

private:
  auto parse_next_token() noexcept -> std::expected<Token, TokenizerError>;

private:
  std::span<std::byte> buffer_;
  std::size_t idx_{};
  std::variant<TokenNumber, TokenOperator> next_token_;
  std::optional<TokenizerError> error_;
};

Tokenizer::Tokenizer(std::span<std::byte> buffer) noexcept
    : buffer_{std::move(buffer)} {}

auto Tokenizer::next() noexcept -> bool {
  if (idx_ >= buffer_.size()) {
    return false;
  }

  auto token_result = parse_next_token();
  if (!token_result) {
    error_ = std::move(token_result.error());
    assert(error_.has_value());

    if (std::holds_alternative<PartialTokenError>(*error_)) {
      assert(idx_ == buffer_.size());

      auto const partial_token_offset =
          std::get<PartialTokenError>(*error_).offset_;

      assert(partial_token_offset < buffer_.size());

      std::memmove(buffer_.data(), buffer_.data() + partial_token_offset,
                   buffer_.size() - partial_token_offset);
    }

    return false;
  }

  auto token = std::move(*token_result);

  if (std::holds_alternative<TokenEmpty>(token)) {
    return false;
  }

  if (std::holds_alternative<TokenEof>(token)) {
    std::memmove(buffer_.data(), buffer_.data() + idx_, buffer_.size() - idx_);
    return false;
  }

  if (std::holds_alternative<TokenNumber>(token)) {
    next_token_ = std::move(std::get<TokenNumber>(token));
  }

  if (std::holds_alternative<TokenOperator>(token)) {
    next_token_ = std::move(std::get<TokenOperator>(token));
  }

  return true;
}

auto Tokenizer::get() const noexcept
    -> std::variant<TokenNumber, TokenOperator> const & {
  return next_token_;
}

auto Tokenizer::get_error() const noexcept
    -> std::optional<TokenizerError> const & {
  return error_;
}

auto Tokenizer::parse_next_token() noexcept
    -> std::expected<Token, TokenizerError> {
  assert(buffer_.size() > 0);
  assert(idx_ < buffer_.size());

  auto is_operator = [](char c) noexcept {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '(' || c == ')';
  };

  char const c = static_cast<char>(buffer_[idx_]);

  if (std::isspace(c)) {
    auto const non_empty_char_ptr =
        std::find_if_not(buffer_.data() + idx_, buffer_.data() + buffer_.size(),
                         [](auto const c) noexcept {
                           return std::isspace(static_cast<char>(c));
                         });
    idx_ += non_empty_char_ptr - buffer_.data();
    if (idx_ == buffer_.size()) {
      return TokenEmpty{};
    }
  }

  if (std::isdigit(c)) {
    auto const token_end_ptr =
        std::find_if_not(buffer_.data() + idx_, buffer_.data() + buffer_.size(),
                         [](auto const c) noexcept {
                           return std::isdigit(static_cast<char>(c));
                         });

    if (token_end_ptr == buffer_.data() + buffer_.size()) {
      return std::unexpected(
          PartialTokenError{std::exchange(idx_, buffer_.size())});
    }

    int64_t value;
    std::from_chars(reinterpret_cast<char *>(buffer_.data() + idx_),
                    reinterpret_cast<char *>(token_end_ptr), value);

    return TokenNumber{value};
  }

  if (is_operator(c)) {
    ++idx_;
    return TokenOperator{c};
  }

  if (c == FRAME_DELIM) {
    ++idx_;
    return TokenEof{};
  }

  return std::unexpected(UnknownTokenError{idx_});
}

struct EvalErrorDividedByZero {};
struct EvalErrorInsufficientOperandsAmount {};

using EvalError =
    std::variant<EvalErrorDividedByZero, EvalErrorInsufficientOperandsAmount>;

class EvaluationEngine {
public:
  using on_done_cb_t = std::function<void(int64_t)>;
  [[nodiscard]]
  auto eval(std::span<std::byte> buffer) noexcept -> std::size_t;
  auto on_done(on_done_cb_t) noexcept -> void;

private:
  static constexpr auto get_precedence(char op) noexcept -> uint16_t;
  static constexpr auto apply(char op, int64_t v1, int64_t v2) noexcept
      -> std::expected<int64_t, EvalErrorDividedByZero>;

  auto pop_operator_and_eval() noexcept -> std::expected<void, EvalError>;

private:
  std::deque<int64_t> queue_;
  std::stack<char> op_stack_;
  on_done_cb_t on_done_;
  bool should_skip_frame_{};
};

auto EvaluationEngine::eval(std::span<std::byte> buffer) noexcept
    -> std::size_t {
  if (should_skip_frame_) {
    auto ptr = std::find_if(
        buffer.data(), buffer.data() + buffer.size(),
        [](auto const c) noexcept { return c == std::byte{FRAME_DELIM}; });
    if (ptr != buffer.data() + buffer.size()) {
      should_skip_frame_ = false;
      auto const unhandled_size = buffer.data() + buffer.size() - ptr;
      std::memmove(buffer.data(), ptr, unhandled_size);
      return unhandled_size;
    }

    return 0;
  }

  Tokenizer tokenizer{buffer};

  while (tokenizer.next()) {
    auto const &token = tokenizer.get();
    if (std::holds_alternative<TokenNumber>(token)) {
      queue_.emplace_back(std::get<TokenNumber>(token).value_);
    } else {
      assert(std::holds_alternative<TokenOperator>(token));
      if (queue_.empty()) {
        should_skip_frame_ = true;
        // TODO
        throw std::runtime_error("Unimplented");
      }

      auto const op = std::get<TokenNumber>(token).value_;
      switch (op) {
      case '(':
        op_stack_.push(op);
        break;
      case ')':
        while (!op_stack_.empty() && op_stack_.top() != '(') {
          auto result = pop_operator_and_eval();
          if (!result) {
            // TODO
            throw std::runtime_error("Unimplented");
          }
        }

        if (op_stack_.empty()) {
          // TODO
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
            throw std::runtime_error("Unimplented");
          }
        }

        op_stack_.push(op);
        break;
      }
    }
  }

  if (auto err = tokenizer.get_error(); err) {
    if (std::holds_alternative<PartialTokenError>(*err)) {
    }
  }

  // Eof
  while (!op_stack_.empty()) {
    auto result = pop_operator_and_eval();
    if (!result) {
      // TODO
      throw std::runtime_error("Unimplented");
    }
  }

  assert(queue_.size() > 0);

  if (queue_.size() != 1) {
    throw std::runtime_error("Unimplented");
  }

  if (on_done_) {
    auto result = queue_.back();
    queue_.pop_back();
    on_done_(result);
  }

  return 0;
}

auto EvaluationEngine::on_done(on_done_cb_t cb) noexcept -> void {
  on_done_ = cb;
}

constexpr auto EvaluationEngine::get_precedence(char op) noexcept -> uint16_t {
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
    -> std::expected<int64_t, EvalErrorDividedByZero> {
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

auto EvaluationEngine::pop_operator_and_eval() noexcept
    -> std::expected<void, EvalError> {
  if (queue_.size() < 2) {
    return std::unexpected(EvalErrorInsufficientOperandsAmount{});
  }

  auto v2 = queue_.back();
  queue_.pop_back();

  auto v1 = queue_.back();
  queue_.pop_back();

  auto apply_result = apply(op_stack_.top(), v1, v2);
  if (!apply_result) {
    return std::unexpected(std::move(apply_result.error()));
  }

  queue_.push_back(*apply_result);
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

Connection::Connection(tcp::socket socket) noexcept
    : socket_{std::move(socket)} {
  auto exception_handler = [](std::exception_ptr e) {
    if (e) {
      try {
        std::rethrow_exception(e);
      } catch (std::exception const &e) {
        logger::error("Exception: ", e.what());
      }
    }
  };

  engine_.on_done([this](auto result) {
    logger::debug("Write back result value: {}", result);

    asio::post(socket_.get_executor(), [this, result]() {
      // int64_t is at most 20 characters long
      std::array<std::byte, 24> reply_frame;
      std::snprintf(reinterpret_cast<char *>(reply_frame.data()),
                    reply_frame.size(), "%ld\n", result);
      asio::async_write(
          socket_, asio::buffer(reply_frame), [](std::error_code ec, auto) {
            if (ec) {
              logger::error("Write back result failed: ", ec.message());
            }
          });
    });
  });

  asio::co_spawn(
      socket.get_executor(), [this]() { return serve(); }, exception_handler);
}

auto Connection::on_close(on_close_cb_t cb) noexcept -> void { on_close_ = cb; }

auto Connection::serve() -> asio::awaitable<void> {
  std::size_t offset = 0;

  while (true) {
    auto const n_bytes = co_await socket_.async_read_some(
        asio::buffer(buffer_.data() + offset, buffer_.size() - offset));
    offset = engine_.eval({buffer_.data(), n_bytes});
  }
}

class TcpServer {
public:
  using executor_type = asio::io_context::executor_type;

  TcpServer() noexcept = default;
  ~TcpServer();
  auto stop() noexcept -> void;
  auto listen_and_serve(asio::io_context &ctx, std::string_view address,
                        uint16_t port) noexcept -> void;

private:
  std::list<std::unique_ptr<Connection>> connections_;
  bool is_stopped_{false};
};

class WorkerThreadManager {
public:
  using task_t = std::function<void()>;

  WorkerThreadManager(int32_t num_threads = std::max(
                          std::thread::hardware_concurrency(), 1U)) noexcept;

  ~WorkerThreadManager();

  auto serve(std::string_view address, uint16_t port) noexcept -> void;
  auto stop() noexcept -> void;

private:
  using work_guard_t =
      asio::executor_work_guard<asio::io_context::executor_type>;

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

SignalListener::SignalListener(
    WorkerThreadManager &worker_thread_manager) noexcept
    : worker_thread_manager_{worker_thread_manager} {}

auto SignalListener::run() noexcept -> void {
  asio::co_spawn(
      ctx_, [this]() noexcept { return handle_signal(); }, asio::detached);

  // TODO: remove this and implement signal handler logic
  auto work = asio::make_work_guard(ctx_);

  ctx_.run();
}

auto SignalListener::handle_signal() -> asio::awaitable<void> {
  // TODO: handle signal handler logic
  co_return;
}

WorkerThreadManager::WorkerThread::WorkerThread() noexcept
    : tcp_server_{}, work_guard_{asio::make_work_guard(ctx_)} {}

auto WorkerThreadManager::WorkerThread::serve(std::string_view address,
                                              uint16_t port) noexcept -> void {
  thread_ = std::thread{[this, address, port]() {
    auto tid = get_thread_id();
    logger::debug("Thread {} start", tid);
    tcp_server_.listen_and_serve(ctx_, address, port);
    logger::debug("Thread {} done", tid);
  }};
}

auto WorkerThreadManager::WorkerThread::join() noexcept -> void {
  work_guard_.reset();
  thread_.join();
}

WorkerThreadManager::WorkerThreadManager(int32_t num_threads) noexcept {
  worker_threads_.reserve(num_threads);

  for (auto const _ : std::views::iota(0, num_threads)) {
    worker_threads_.emplace_back(
        std::make_unique<WorkerThreadManager::WorkerThread>());
  }
}

WorkerThreadManager::~WorkerThreadManager() { stop(); }

auto WorkerThreadManager::serve(std::string_view address,
                                uint16_t port) noexcept -> void {
  for (auto &worker_thread : worker_threads_) {
    worker_thread->serve(address, port);
  }

  logger::info("Tcp server listen and serve at {}:{}", address, port);
}

auto WorkerThreadManager::stop() noexcept -> void {
  if (std::exchange(is_stopped_, true)) {
    return;
  }

  for (auto &worker_thread : worker_threads_) {
    worker_thread->join();
  }
}

TcpServer::~TcpServer() { stop(); }

auto TcpServer::stop() noexcept -> void {
  if (std::exchange(is_stopped_, true)) {
    return;
  }
}

auto TcpServer::listen_and_serve(asio::io_context &ctx,
                                 std::string_view address,
                                 uint16_t port) noexcept -> void {
  asio::co_spawn(
      ctx,
      [this, exec = ctx.get_executor(), address,
       port]() -> asio::awaitable<void> {
        tcp::acceptor acceptor{exec};
        tcp::endpoint endpoint{asio::ip::make_address(address), port};

        acceptor.open(endpoint.protocol());

        // Layer 4 load balancing connections to threads
        int optval = 1;
        ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                     &optval, sizeof(optval));

        acceptor.set_option(tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen();

        while (true) {
          try {
            tcp::socket socket{exec};
            socket.set_option(tcp::no_delay(true));

            co_await acceptor.async_accept(socket);

            auto &conn = connections_.emplace_front(
                std::make_unique<Connection>(std::move(socket)));

            conn->on_close([this, it = connections_.begin()]() noexcept {
              connections_.erase(it);
            });

            logger::debug("Thread id = {} accept connection", get_thread_id());

          } catch (const std::exception &e) {
            logger::error("Exception while listening for connections: {}",
                          e.what());
          }
        }
      },
      asio::detached);

  ctx.run();
}

int main(int argc, char **argv) {
  spdlog::cfg::load_env_levels();

  uint16_t const port = (argc == 2) ? std::stoi(argv[1]) : 8000;

  WorkerThreadManager worker_thread_manager{};

  worker_thread_manager.serve("0.0.0.0", port);

  SignalListener signal_listener{worker_thread_manager};
  signal_listener.run();
}
