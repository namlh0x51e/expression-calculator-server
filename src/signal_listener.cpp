#include "signal_listener.hpp"

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
        spdlog::info("Received signal {}, shutting down", sig);
        worker_thread_manager_.stop();
    }
}
