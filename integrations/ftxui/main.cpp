#include "app.hpp"
#include <filesystem>
#include <iostream>
#include <future>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
namespace {
std::filesystem::path executable(const char* argv0) {
#ifdef __linux__
    return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::string path(length, '\0');
    if (_NSGetExecutablePath(path.data(), &length) == 0) return std::filesystem::canonical(path.c_str());
#endif
    return std::filesystem::absolute(argv0);
}
}
int main(int argc, char** argv) {
    try {
        std::string socket = "/tmp/sosd.sock";
        auto daemon = executable(argv[0]).parent_path() / "sosd";
        bool check = false;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--help") {
                std::cout << "Usage: sos-ftxui [--socket PATH] [--daemon PATH] [--check]\n"
                             "Native C++/FTXUI integration. --check connects, configures and exits.\n";
                return 0;
            }
            if ((argument == "--socket" || argument == "--daemon") && i + 1 < argc) {
                if (argument == "--socket") socket = argv[++i]; else daemon = argv[++i];
            } else if (argument == "--check") check = true;
            else throw std::runtime_error("Unknown or incomplete option: " + argument);
        }
        sos::tui::Client client;
        auto hello = client.connect(socket, daemon);
        if (check) {
            auto reply = std::make_shared<std::promise<sos::tui::Json>>();
            auto future = reply->get_future();
            client.start([reply, received = false](sos::tui::Json event) mutable {
                if (!received) { received = true; reply->set_value(std::move(event)); }
            });
            client.send({{"type", "integration.configure"}, {"workspace", std::filesystem::current_path().string()}, {"mode", "sane"}});
            if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) throw std::runtime_error("Daemon configuration timed out");
            const auto event = future.get();
            if (event.value("type", "") != "integration.configured") throw std::runtime_error(event.value("error", "Daemon configuration failed"));
            std::cout << "Connected to daemon protocol " << hello.at("protocolVersion") << '\n';
            return 0;
        }
        if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) throw std::runtime_error("An interactive terminal is required (use --check for a connection check)");
        sos::tui::App app(client, std::move(hello), std::filesystem::current_path().string());
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "sos-ftxui: " << error.what() << '\n';
        return 1;
    }
}
