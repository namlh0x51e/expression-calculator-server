#include <spdlog/cfg/env.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "signal_listener.hpp"
#include "worker_thread_manager.hpp"

static_assert(sizeof(std::byte) == sizeof(unsigned char));

int main(int argc, char **argv)
{
    spdlog::cfg::load_env_levels();

    std::string host = "0.0.0.0";
    uint16_t port = 8000;
    uint32_t num_threads = std::max(std::thread::hardware_concurrency(), 1U);

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--threads" && i + 1 < argc) {
            num_threads = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            fmt::println(
                "Usage: {} [options]\n"
                "  --host HOST  Server hostname/IP.\n"
                "  --port PORT  Server port.\n"
                "  --threads N  Number of IO threads (default to number of cpus).\n"
                "  --help N     Print this message and exit.",
                argv[0]);
            return 0;
        }
    }

    WorkerThreadManager worker_thread_manager{num_threads};

    worker_thread_manager.serve(host, port);

    SignalListener signal_listener{worker_thread_manager};
    signal_listener.run();
}
