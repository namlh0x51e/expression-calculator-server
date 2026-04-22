#pragma once

#include "evaluation_engine/evaluation_engine.hpp"

#include <asio.hpp>

using asio::ip::tcp;

constexpr auto BUFFER_SIZE = 1024 * 1024;

class Connection : public std::enable_shared_from_this<Connection> {
   public:
    using on_close_cb_t = std::function<void()>;

    static auto make(tcp::socket socket) noexcept -> std::shared_ptr<Connection>;

    auto on_close(on_close_cb_t) noexcept -> void;
    auto cancel() noexcept -> void;

   private:
    explicit Connection(tcp::socket socket) noexcept;
    auto start() noexcept -> void;
    auto serve() -> asio::awaitable<void>;

   private:
    tcp::socket socket_;
    EvaluationEngine engine_;
    std::array<std::byte, BUFFER_SIZE> buffer_;

    on_close_cb_t on_close_;
    asio::cancellation_signal cancel_signal_;
};
