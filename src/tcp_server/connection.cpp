#include "connection.hpp"

#include <spdlog/fmt/bin_to_hex.h>

#include <charconv>

auto Connection::make(tcp::socket socket) noexcept -> std::shared_ptr<Connection>
{
    // The constructor is private
    auto conn = std::shared_ptr<Connection>(new Connection{std::move(socket)});
    conn->start();
    return conn;
}

Connection::Connection(tcp::socket socket) noexcept : socket_{std::move(socket)} {}

auto Connection::start() noexcept -> void
{
    engine_.on_done([weak = weak_from_this()](auto result) {
        auto self = weak.lock();
        if (!self) return;
        asio::post(self->socket_.get_executor(), [self, result]() {
            if (result) {
                spdlog::debug("Write back result value: {}", *result);
                // int64_t is only 20 characters long
                auto reply = std::make_shared<std::array<char, 24>>();
                auto to_chars_res = std::to_chars(reply->data(), reply->data() + reply->size(), *result);
                auto n = static_cast<std::size_t>(to_chars_res.ptr - reply->data());
                assert(n >= 0);
                assert(n < reply.size() - 1);  // Count for '\n'
                (*reply)[n++] = '\n';
                asio::async_write(self->socket_, asio::buffer(reply->data(), n),
                                  [self, reply](std::error_code ec, auto) mutable {
                                      if (ec) spdlog::error("Write back result failed: {}", ec.message());
                                  });
            } else {
                spdlog::debug("Write back result value: \"SYNTAX ERROR\"");
                asio::async_write(self->socket_, asio::buffer("SYNTAX ERROR\n"), [self](std::error_code ec, auto) {
                    if (ec) spdlog::error("Write back result failed: {}", ec.message());
                });
            }
        });
    });

    asio::co_spawn(
        socket_.get_executor(), [self = shared_from_this()]() { return self->serve(); },
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
            spdlog::debug("Byte stream received: {}",
                          spdlog::to_hex(buffer_.data() + offset, buffer_.data() + offset + n_bytes));
            offset = engine_.eval({buffer_.data(), n_bytes});
        }
    } catch (std::system_error const &e) {
        if (e.code() == asio::error::eof) {
            spdlog::info("Connection closed by peer");
        } else if (e.code() != asio::error::operation_aborted) {
            spdlog::error("Exception system error: {}", e.code().message());
        }
    } catch (std::exception const &e) {
        spdlog::error("Exception: {}", e.what());
    }
    if (on_close_) on_close_();
}
