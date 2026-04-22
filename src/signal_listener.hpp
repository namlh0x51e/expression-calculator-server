#pragma once

#include "worker_thread_manager.hpp"

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

