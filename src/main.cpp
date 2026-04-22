#include <spdlog/cfg/env.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>

#include "worker_thread_manager.hpp"
#include "signal_listener.hpp"

using namespace std::string_view_literals;

static_assert(sizeof(std::byte) == sizeof(unsigned char));

int main(int argc, char **argv)
{
    spdlog::cfg::load_env_levels();

    uint16_t const port = (argc == 2) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8000;

    WorkerThreadManager worker_thread_manager{};

    worker_thread_manager.serve("0.0.0.0", port);

    SignalListener signal_listener{worker_thread_manager};
    signal_listener.run();
}
