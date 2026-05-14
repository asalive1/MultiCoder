// worker/main.cpp — MultiCoder Worker Entry Point
// One instance per encoder. Reads config from /etc/encoder{N}/ and manages
// audio input, encoding (AAC/MP3), and output sinks (Icecast, HLS, SRT).
// IPC with supervisor over a localhost HTTP socket.

#include "worker.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#include <direct.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char** argv) {
#ifdef _WIN32
    // Ensure /etc/... paths resolve to C:\etc\... regardless of launch CWD.
    _chdrive('C' - 'A' + 1);
    _chdir("\\");
#endif
    if (argc < 2) {
        std::cerr << "Usage: multicoder-worker <encoderIndex 1-5>\n";
        return 1;
    }
    int idx = std::stoi(argv[1]);
    if (idx < 1 || idx > 5) {
        std::cerr << "encoderIndex must be 1-5\n";
        return 1;
    }

    std::string cfgDir = "/etc/encoder" + std::to_string(idx);
    std::string logDir = cfgDir + "/logs";
    std::string hlsDir = cfgDir + "/hls";

    fs::create_directories(logDir);
    fs::create_directories(hlsDir + "/segments");

    std::cout << "multicoder-worker[" << idx << "] starting. config=" << cfgDir << "\n";

    Worker w(idx, cfgDir);
    w.run();  // blocks until signalled

    return 0;
}
