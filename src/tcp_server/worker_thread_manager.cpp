#include "worker_thread_manager.hpp"
#include <sched.h>
#include <spdlog/spdlog.h>
#include <ranges>

auto get_thread_id() noexcept -> std::string
{
    auto tid = std::this_thread::get_id();
    std::stringstream ss;
    ss << tid;
    return ss.str();
}

WorkerThreadManager::WorkerThread::WorkerThread(std::size_t cpu_core_id) noexcept
    : cpu_core_id_{cpu_core_id}, tcp_server_{}, work_guard_{asio::make_work_guard(ctx_)}
{
}

auto WorkerThreadManager::WorkerThread::serve(std::string_view address, uint16_t port) noexcept -> void
{
    thread_ = std::thread{[this, address, port]() {
        auto tid = get_thread_id();
        spdlog::debug("Thread {} start", tid);
        tcp_server_.listen_and_serve(ctx_, address, port);
        spdlog::debug("Thread {} done", tid);
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
        spdlog::warn("Failed to set thread affinity: {}", ::strerror(errno));
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

    spdlog::info("Tcp server listen and serve at {}:{}", address, port);
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

