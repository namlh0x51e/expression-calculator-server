#include "tcp_server.hpp"
#include <utility>

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

                    auto conn = Connection::make(std::move(socket));
                    connections_.emplace_front(conn);

                    conn->on_close([this, it = connections_.begin()]() noexcept { connections_.erase(it); });

                    // spdlog::debug("Thread id = {} accept connection", get_thread_id());
                }
            } catch (const asio::system_error &e) {
                if (e.code() != asio::error::operation_aborted) {
                    spdlog::error("Exception while listening for connections: {}", e.what());
                }
            } catch (const std::exception &e) {
                spdlog::error("Exception while listening for connections: {}", e.what());
            }
        },
        asio::bind_cancellation_slot(cancel_signal_.slot(), asio::detached));

    ctx.run();
}
