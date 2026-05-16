#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include "supervisor_api.h"
#ifdef _WIN32
#include <direct.h>  // _chdir, _chdrive
#endif

static std::atomic<bool> g_svShutdown{false};
static void svSigHandler(int) { g_svShutdown = true; }

static std::string envValue(const char* name) {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf) return "";
    std::string out(buf);
    free(buf);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

int main(int argc, char** argv) {
    std::cout << "MultiCoder Supervisor starting...\n";

#ifdef _WIN32
    // When launched from PowerShell with a UNC working directory (e.g.
    // \\server\share\...), paths like "/etc/encoder1/" resolve to
    // \\server\share\etc\encoder1\ instead of C:\etc\encoder1\.
    // Force the current directory to C:\ so all /etc/... paths resolve correctly.
    _chdrive('C' - 'A' + 1);
    _chdir("\\");
#endif

    signal(SIGTERM, svSigHandler);
    signal(SIGINT,  svSigHandler);

    std::filesystem::create_directories("/etc/MC");

    int port = 8050;
    std::string envPort = envValue("UI_PORT");
    if (!envPort.empty()) {
        int parsed = std::atoi(envPort.c_str());
        if (parsed > 0 && parsed < 65536) port = parsed;
    }

    start_supervisor_api(port);

    while (!g_svShutdown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "MultiCoder Supervisor shutting down...\n";
    stop_supervisor_api();
    return 0;
}
