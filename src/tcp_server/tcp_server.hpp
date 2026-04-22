#pragma once

#include <asio.hpp>
#include <list>
#include "connection.hpp"

class TcpServer {
   public:
    using executor_type = asio::io_context::executor_type;

    TcpServer() noexcept = default;
    ~TcpServer();
    auto stop() noexcept -> void;
    auto listen_and_serve(asio::io_context &ctx, std::string_view address, uint16_t port) noexcept -> void;

   private:
    std::list<std::shared_ptr<Connection>> connections_;
    asio::cancellation_signal cancel_signal_;
    bool is_stopped_{false};
};
