#include <sched.h>
#include <spdlog/cfg/env.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <list>
#include <memory>
#include <ranges>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "asio/detached.hpp"
#include "evaluation_engine.hpp"

using asio::ip::tcp;

using namespace std::string_view_literals;

static_assert(sizeof(std::byte) == sizeof(unsigned char));

constexpr auto BUFFER_SIZE = 1024 * 1024;

namespace logger = spdlog;

auto get_thread_id() noexcept -> std::string
{
    auto tid = std::this_thread::get_id();
    std::stringstream ss;
    ss << tid;
    return ss.str();
}

class Connection {
   public:
    using on_close_cb_t = std::function<void()>;

    Connection(tcp::socket socket) noexcept;

    auto on_close(on_close_cb_t) noexcept -> void;
    auto cancel() noexcept -> void;

   private:
    auto serve() -> asio::awaitable<void>;

   private:
    tcp::socket socket_;
    EvaluationEngine engine_;
    std::array<std::byte, BUFFER_SIZE> buffer_;

    on_close_cb_t on_close_;
    asio::cancellation_signal cancel_signal_;
};

Connection::Connection(tcp::socket socket) noexcept : socket_{std::move(socket)}
{
    engine_.on_done([this](auto result) {
        asio::post(socket_.get_executor(), [this, result]() {
            // int64_t is at most 20 characters long
            std::array<std::byte, 24> reply_frame{};
            int32_t n;

            if (result) {
                logger::debug("Write back result value: {}", *result);
                n = std::snprintf(reinterpret_cast<char *>(reply_frame.data()), reply_frame.size(), "%ld\n", *result);
            } else {
                logger::debug("Write back result value: \"SYNTAX ERROR\"");
                n = std::snprintf(reinterpret_cast<char *>(reply_frame.data()), reply_frame.size(), "SYNTAX ERROR\n");
            }

            asio::async_write(socket_, asio::buffer(reply_frame.data(), static_cast<std::size_t>(n)),
                              [](std::error_code ec, auto) {
                                  if (ec) {
                                      logger::error("Write back result failed: {}", ec.message());
                                  }
                              });
        });
    });

    asio::co_spawn(socket_.get_executor(), [this]() { return serve(); },
                   asio::bind_cancellation_slot(cancel_signal_.slot(), asio::detached));
}

auto Connection::on_close(on_close_cb_t cb) noexcept -> void { on_close_ = cb; }

auto Connection::cancel() noexcept -> void { cancel_signal_.emit(asio::cancellation_type::terminal); }

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
    } catch (std::system_error const &e) {
        if (e.code() != asio::error::operation_aborted) {
            logger::error("Exception system error: {}", e.code().message());
        }
    } catch (std::exception const &e) {
        logger::error("Exception: {}", e.what());
    }
    if (on_close_) on_close_();
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
    asio::cancellation_signal cancel_signal_;
    bool is_stopped_{false};
};

class WorkerThreadManager {
   public:
    using task_t = std::function<void()>;

    WorkerThreadManager(uint32_t num_threads = std::max(std::thread::hardware_concurrency(), 1U)) noexcept;

    ~WorkerThreadManager();

    auto serve(std::string_view address, uint16_t port) noexcept -> void;
    auto stop() noexcept -> void;

   private:
    using work_guard_t = asio::executor_work_guard<asio::io_context::executor_type>;

    struct WorkerThread {
        std::size_t cpu_core_id_;
        asio::io_context ctx_;
        TcpServer tcp_server_;
        std::thread thread_;
        work_guard_t work_guard_;

        WorkerThread(std::size_t cpu_core_id) noexcept;

        auto serve(std::string_view address, uint16_t port) noexcept -> void;
        auto join() noexcept -> void;
        auto set_affinity() noexcept -> void;
    };

    std::vector<std::unique_ptr<WorkerThread>> worker_threads_;

    bool is_stopped_{false};
};

class SignalListener {
   public:
    SignalListener(WorkerThreadManager &worker_thread_manager) noexcept;

    auto run() noexcept -> void;

   private:
    auto handle_signal() noexcept -> asio::awaitable<void>;

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
    ctx_.run();
}

auto SignalListener::handle_signal() noexcept -> asio::awaitable<void>
{
    asio::signal_set signals{ctx_, SIGINT, SIGTERM};
    auto [ec, sig] = co_await signals.async_wait(asio::as_tuple(asio::use_awaitable));
    if (!ec) {
        logger::info("Received signal {}, shutting down", sig);
        worker_thread_manager_.stop();
    }
}

WorkerThreadManager::WorkerThread::WorkerThread(std::size_t cpu_core_id) noexcept
    : cpu_core_id_{cpu_core_id}, tcp_server_{}, work_guard_{asio::make_work_guard(ctx_)}
{
}

auto WorkerThreadManager::WorkerThread::serve(std::string_view address, uint16_t port) noexcept -> void
{
    thread_ = std::thread{[this, address, port]() {
        auto tid = get_thread_id();
        logger::debug("Thread {} start", tid);
        tcp_server_.listen_and_serve(ctx_, address, port);
        logger::debug("Thread {} done", tid);
    }};

    set_affinity();
}

auto WorkerThreadManager::WorkerThread::join() noexcept -> void
{
    tcp_server_.stop();   // cancel accept loop; in-flight connections keep running
    work_guard_.reset();  // io_context exits when all connections close
    thread_.join();
}

auto WorkerThreadManager::WorkerThread::set_affinity() noexcept -> void
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_core_id_, &cpuset);
    int rc = pthread_setaffinity_np(thread_.native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        logger::warn("Failed to set thread affinity: {}", ::strerror(errno));
    }
}

WorkerThreadManager::WorkerThreadManager(uint32_t num_threads) noexcept
{
    worker_threads_.reserve(num_threads);

    for (auto const i : std::views::iota(0U, num_threads)) {
        worker_threads_.emplace_back(std::make_unique<WorkerThreadManager::WorkerThread>(i));
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
    for (auto &conn : connections_) {
        conn->cancel();
    }
    cancel_signal_.emit(asio::cancellation_type::terminal);
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

            try {
                while (true) {
                    tcp::socket socket{exec};

                    co_await acceptor.async_accept(socket);

                    socket.set_option(tcp::no_delay(true));

                    auto &conn = connections_.emplace_front(std::make_unique<Connection>(std::move(socket)));

                    conn->on_close([this, it = connections_.begin()]() noexcept { connections_.erase(it); });

                    logger::debug("Thread id = {} accept connection", get_thread_id());
                }
            } catch (const asio::system_error &e) {
                if (e.code() != asio::error::operation_aborted) {
                    logger::error("Exception while listening for connections: {}", e.what());
                }
            } catch (const std::exception &e) {
                logger::error("Exception while listening for connections: {}", e.what());
            }
        },
        asio::bind_cancellation_slot(cancel_signal_.slot(), asio::detached));

    ctx.run();
}

int main(int argc, char **argv)
{
    spdlog::cfg::load_env_levels();

    uint16_t const port = (argc == 2) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8000;

    WorkerThreadManager worker_thread_manager{};

    worker_thread_manager.serve("0.0.0.0", port);

    SignalListener signal_listener{worker_thread_manager};
    signal_listener.run();
}
