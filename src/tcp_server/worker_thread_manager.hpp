#pragma once 

#include <asio.hpp>
#include "tcp_server.hpp"

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
