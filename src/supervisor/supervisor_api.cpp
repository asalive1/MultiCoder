// supervisor_api.cpp - MultiCoder Supervisor HTTP API server

#include "supervisor_api.h"
#include "SimpleJson.h"
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <mutex>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <ctime>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#pragma comment(lib, "Ws2_32.lib")
typedef int ssize_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
static void close_socket(int fd) { closesocket((SOCKET)fd); }
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
static void initSocketsOnce() {
    static bool inited = false;
    if (!inited) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        inited = true;
    }
}
#else
static void close_socket(int fd) { close(fd); }
static void initSocketsOnce() {}
#endif

static const int MAX_ENCODERS = 5;

// ---------- shutdown / worker-tracking state ----------
static std::atomic<bool> g_svAcceptRunning{true};
static int g_listenSocket = -1;
#ifdef _WIN32
static std::mutex g_workerHandlesMutex;
static std::map<int, HANDLE> g_workerHandles;  // keyed by 1-based encoder index

// Windows Job Object: any child process added here is automatically killed
// when the supervisor process exits (even if killed hard by the OS / debugger).
static HANDLE getOrCreateJobObject() {
    static HANDLE hJob = INVALID_HANDLE_VALUE;
    if (hJob != INVALID_HANDLE_VALUE) return hJob;
    hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob && hJob != INVALID_HANDLE_VALUE) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }
    return hJob;
}
#endif
// ---------------------------------------------------------

static std::string readFile(const std::string& path);

struct EncoderState {
    bool aac_running = false;
    bool mp3_running = false;
    bool hls_running = false;
    bool srt_running = false;
};

static std::mutex g_stateMutex;
static EncoderState g_encoders[MAX_ENCODERS];

static std::string runtimeStatePath(int encoderOneBased) {
    return "/etc/encoder" + std::to_string(encoderOneBased) + "/runtime_state.json";
}

static std::string nowIsoUtc() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

static void appendSysLog(int encoderOneBased, const std::string& message) {
    fs::create_directories("/etc/MC");
    std::ofstream f("/etc/MC/EncoderSys.log", std::ios::app);
    if (!f.is_open()) return;
    f << "[" << nowIsoUtc() << "] [Supervisor][Encoder-" << encoderOneBased << "] " << message << "\n";
}

static void appendEncoderLog(int encoderOneBased, const std::string& message) {
    std::string dir = "/etc/encoder" + std::to_string(encoderOneBased) + "/logs";
    fs::create_directories(dir);
    std::string path = dir + "/Encoder" + std::to_string(encoderOneBased) + ".log";
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) return;
    f << "[" << nowIsoUtc() << "] [Supervisor] " << message << "\n";
}

static bool testTcpConnect(const std::string& host, int port, std::string& err) {
    if (host.empty() || port <= 0 || port > 65535) {
        err = "invalid host/port";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (gai != 0 || !res) {
        err = "DNS/resolve failed";
        return false;
    }

    bool ok = false;
    err = "connect failed";
    for (addrinfo* p = res; p; p = p->ai_next) {
        int s = static_cast<int>(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (s < 0) continue;

#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket((SOCKET)s, FIONBIO, &mode);
#else
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

        int rc = connect(s, p->ai_addr, static_cast<int>(p->ai_addrlen));
        if (rc == 0) {
            ok = true;
            close_socket(s);
            break;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        timeval tv{};
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        int sel = select(s + 1, nullptr, &wfds, nullptr, &tv);
        if (sel > 0 && FD_ISSET(s, &wfds)) {
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &slen);
            if (soerr == 0) {
                ok = true;
                close_socket(s);
                break;
            }
        }
        close_socket(s);
    }

    freeaddrinfo(res);
    if (!ok && err.empty()) err = "connect timeout";
    return ok;
}

static simplejson::Object readRuntimeState(int encoderOneBased) {
    simplejson::Object o;
    std::string txt = readFile(runtimeStatePath(encoderOneBased));
    if (!txt.empty()) o.parse(txt);
    return o;
}

// Convert a Unix-style /etc/... path to a Windows absolute path with backslashes.
// On Windows, std::filesystem and CreateFileW may not resolve leading '/' paths
// the same way as the CRT fopen/ofstream does.  MoveFileExW requires the resolved
// absolute path to work reliably.
static std::string toWindowsPath(const std::string& p) {
#ifdef _WIN32
    // Resolve the logical path through the CRT so we get the same drive mapping
    // as std::ofstream.  _fullpath resolves relative/virtual paths to absolute.
    char resolved[MAX_PATH] = {};
    if (_fullpath(resolved, p.c_str(), MAX_PATH)) return std::string(resolved);
    // Fallback: manual slash conversion
    std::string r = p;
    for (char& c : r) if (c == '/') c = '\\';
    return r;
#else
    return p;
#endif
}

// Write runtime state atomically: write to .sv.tmp then MoveFileExW so concurrent
// readers never see a truncated/empty file.
static void atomicWriteRuntimeState(int encoderOneBased, const simplejson::Object& o) {
    std::string path    = runtimeStatePath(encoderOneBased);
    std::string tmpPath = path + ".sv.tmp";  // .sv.tmp = supervisor; worker uses .wk.tmp
    std::string dir     = "/etc/encoder" + std::to_string(encoderOneBased);
    fs::create_directories(dir);
    {
        std::ofstream f(tmpPath, std::ios::trunc);
        if (!f.is_open()) return;  // directory not writable; silently skip
        f << o.serialize();
    } // closed/flushed here
#ifdef _WIN32
    // Use Win32 MoveFileExW with properly resolved absolute paths.
    // std::filesystem::rename may pass '/'-prefixed paths to MoveFileExW verbatim;
    // toWindowsPath resolves them to C:\etc\... via _fullpath.
    std::string winTmp = toWindowsPath(tmpPath);
    std::string winDst = toWindowsPath(path);
    std::wstring wTmp(winTmp.begin(), winTmp.end());
    std::wstring wDst(winDst.begin(), winDst.end());
    if (!::MoveFileExW(wTmp.c_str(), wDst.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Last-resort fallback: direct overwrite
        std::error_code ec;
        fs::copy_file(tmpPath, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
    }
#else
    std::error_code ec;
    fs::rename(tmpPath, path, ec);
    if (ec) {
        fs::copy_file(tmpPath, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
    }
#endif
}

static void writeRuntimeState(int encoderOneBased, bool controlRunning, bool metadataRunning) {
    simplejson::Object o = readRuntimeState(encoderOneBased);
    o.setBool("controlListenerRunning", controlRunning);
    o.setBool("metadataListenerRunning", metadataRunning);
    atomicWriteRuntimeState(encoderOneBased, o);
}

static void setRuntimeInputConnected(int encoderOneBased, bool connected) {
    simplejson::Object o = readRuntimeState(encoderOneBased);
    o.setBool("inputConnected", connected);
    atomicWriteRuntimeState(encoderOneBased, o);
}

static std::string readFile(const std::string& path) {
#ifdef _WIN32
    // Resolve /etc/... style paths to absolute Windows paths (e.g. C:\etc\...)
    // before calling CreateFileW, since Win32 does not resolve leading '/' paths
    // the same way the CRT fopen/ofstream does.
    char resolvedBuf[MAX_PATH] = {};
    const char* rp = _fullpath(resolvedBuf, path.c_str(), MAX_PATH) ? resolvedBuf : path.c_str();
    std::wstring wpath(rp, rp + strlen(rp));
    // Open with FILE_SHARE_DELETE so atomic renames (MoveFileEx) of this file
    // are not blocked while we hold an open read handle.
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart == 0) { ::CloseHandle(h); return ""; }
    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD bytesRead = 0;
    ::ReadFile(h, &buf[0], static_cast<DWORD>(sz.QuadPart), &bytesRead, nullptr);
    ::CloseHandle(h);
    buf.resize(bytesRead);
    // Strip UTF-8 BOM (0xEF 0xBB 0xBF) if present — avoids invalid JSON when
    // a config file was saved by a Windows editor with "UTF-8 with BOM" encoding.
    if (buf.size() >= 3 &&
        static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) { buf.erase(0, 3); }
    return buf;
#else
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string buf{std::istreambuf_iterator<char>(f), {}};
    // Strip UTF-8 BOM if present.
    if (buf.size() >= 3 &&
        static_cast<unsigned char>(buf[0]) == 0xEF &&
        static_cast<unsigned char>(buf[1]) == 0xBB &&
        static_cast<unsigned char>(buf[2]) == 0xBF) { buf.erase(0, 3); }
    return buf;
#endif
}

static simplejson::Object readJsonFile(const std::string& path) {
    simplejson::Object o;
    std::string txt = readFile(path);
    if (!txt.empty()) o.parse(txt);
    return o;
}

static int controlPortForEncoder(int encoderOneBased) {
    int defPort = 9100 + (encoderOneBased - 1) * 10;
    simplejson::Object ctl = readJsonFile("/etc/encoder" + std::to_string(encoderOneBased) + "/control.json");
    return ctl.getInt("controlPort", defPort);
}

static std::string streamCommandFor(const std::string& streamType, bool start) {
    if (streamType == "aac") return start ? "StartAAC" : "StopAAC";
    if (streamType == "mp3") return start ? "StartMP3" : "StopMP3";
    if (streamType == "hls") return start ? "StartHLS" : "StopHLS";
    if (streamType == "srt") return start ? "StartSRT" : "StopSRT";
    return "";
}

static bool sendWorkerControlCommand(int encoderOneBased, const std::string& command, std::string& err) {
    int port = controlPortForEncoder(encoderOneBased);
    if (command.empty()) {
        err = "empty control command";
        return false;
    }

    int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0) {
        err = "socket() failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        err = "control listener connect failed on 127.0.0.1:" + std::to_string(port);
        return false;
    }

    std::string wire = command + "\n";
    int sent = send(s, wire.c_str(), static_cast<int>(wire.size()), MSG_NOSIGNAL);
    if (sent <= 0) {
        close_socket(s);
        err = "failed to send control command to worker";
        return false;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    int sel = select(s + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0 || !FD_ISSET(s, &rfds)) {
        close_socket(s);
        err = "no ACK from worker control listener";
        return false;
    }

    char ack[64] = {};
    int n = recv(s, ack, sizeof(ack) - 1, 0);
    close_socket(s);
    if (n <= 0) {
        err = "worker closed control socket without ACK";
        return false;
    }

    std::string resp(ack, n);
    if (resp.find("OK") == std::string::npos) {
        err = "worker returned unexpected ACK: " + resp;
        return false;
    }
    return true;
}

// Proxy a single HLS file request to the worker's dedicated HLS HTTP server.
// This ensures clients hitting supervisor /encoder/{N}/hls/* receive the same
// transformed playlist (UTC PROGRAM-DATE-TIME + injected metadata) as direct
// requests to the worker playback port.
static bool proxyHlsFromWorker(int encoderOneBased,
                               const std::string& filename,
                               std::string& body,
                               std::string& err) {
    simplejson::Object adminCfg =
        readJsonFile("/etc/encoder" + std::to_string(encoderOneBased) + "/encoder_admin.json");
    int hlsPort = adminCfg.getInt("hlsPlaybackPort", 0);
    if (hlsPort <= 0) {
        err = "hlsPlaybackPort not configured";
        return false;
    }

    int s = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0) {
        err = "socket() failed";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(hlsPort));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        err = "connect failed";
        return false;
    }

    std::string req = "GET /hls/" + filename + " HTTP/1.0\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n";
    int sent = send(s, req.c_str(), static_cast<int>(req.size()), MSG_NOSIGNAL);
    if (sent <= 0) {
        close_socket(s);
        err = "send failed";
        return false;
    }

    std::string raw;
    char buf[8192];
    while (true) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);
    }
    close_socket(s);

    if (raw.empty()) {
        err = "empty response";
        return false;
    }

    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) {
        err = "bad response";
        return false;
    }
    std::string statusLine = raw.substr(0, lineEnd);
    if (statusLine.find(" 200 ") == std::string::npos) {
        err = "upstream status not 200";
        return false;
    }

    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        err = "bad headers";
        return false;
    }
    body = raw.substr(hdrEnd + 4);
    return true;
}

static std::string workerBinaryPath() {
#ifdef _WIN32
    // First: look next to the supervisor executable itself (most reliable).
    wchar_t exePathW[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePathW, MAX_PATH) > 0) {
        fs::path sibling = fs::path(exePathW).parent_path() / "multicoder-worker.exe";
        if (fs::exists(sibling)) return sibling.string();
    }
    // Fallback: search relative to cwd.
    fs::path cwd = fs::current_path();
    fs::path p1 = cwd / "multicoder-worker.exe";
    if (fs::exists(p1)) return p1.string();
    fs::path p2 = cwd / "src" / "multicoder-worker.exe";
    if (fs::exists(p2)) return p2.string();
    return "multicoder-worker.exe";
#else
    // Search next to this executable first
    {
        char selfBuf[4096] = {};
        ssize_t len = readlink("/proc/self/exe", selfBuf, sizeof(selfBuf) - 1);
        if (len > 0) {
            fs::path sibling = fs::path(std::string(selfBuf, len)).parent_path() / "multicoder-worker";
            if (fs::exists(sibling)) return sibling.string();
        }
    }
    // Docker install path
    if (fs::exists("/opt/multicoder/bin/multicoder-worker"))
        return "/opt/multicoder/bin/multicoder-worker";
    // Fallback: rely on PATH
    return "multicoder-worker";
#endif
}

static bool launchWorkerProcess(int encoderOneBased, std::string& err) {
    if (encoderOneBased < 1 || encoderOneBased > MAX_ENCODERS) {
        err = "invalid encoder index";
        return false;
    }

    std::string workerExe = workerBinaryPath();
#ifdef _WIN32
    if (!fs::exists(workerExe)) {
        err = "worker binary not found: " + workerExe;
        return false;
    }

    // Build a wide command line: "<path\to\worker.exe>" <encoderIdx>
    std::wstring wExe(workerExe.begin(), workerExe.end());
    std::wstring wCmd = L"\"" + wExe + L"\" " + std::to_wstring(encoderOneBased);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_SUSPENDED so we can assign to the Job Object before the process runs.
    BOOL ok = CreateProcessW(
        nullptr, &wCmd[0],
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, nullptr,
        &si, &pi);

    if (!ok) {
        err = "CreateProcessW failed: error " + std::to_string(GetLastError());
        return false;
    }

    // Add to the kill-on-close Job Object so the worker dies if the supervisor exits
    // for any reason (including being killed hard by the OS / VS Code debugger).
    HANDLE hJob = getOrCreateJobObject();
    if (hJob && hJob != INVALID_HANDLE_VALUE) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Store the process handle so stop_supervisor_api() can send an explicit terminate.
    {
        std::lock_guard<std::mutex> lk(g_workerHandlesMutex);
        auto it = g_workerHandles.find(encoderOneBased);
        if (it != g_workerHandles.end()) {
            // A previous worker handle is still stored — close it (process may already be dead).
            CloseHandle(it->second);
            it->second = pi.hProcess;
        } else {
            g_workerHandles[encoderOneBased] = pi.hProcess;
        }
    }
    return true;
#else
    // Linux / Docker: fork + exec the worker binary
    if (!fs::exists(workerExe)) {
        err = "worker binary not found: " + workerExe;
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed";
        return false;
    }
    if (pid == 0) {
        // Child: become the worker process
        std::string idxStr = std::to_string(encoderOneBased);
        execl(workerExe.c_str(), workerExe.c_str(), idxStr.c_str(), nullptr);
        _exit(127);  // execl only returns on error
    }
    // Parent: worker runs in background; do not wait.
    return true;
#endif
}

static bool sendWorkerControlWithAutoStart(int encoderOneBased,
                                           const std::string& command,
                                           bool allowAutoStart,
                                           bool& autoStarted,
                                           std::string& err) {
    autoStarted = false;

    if (sendWorkerControlCommand(encoderOneBased, command, err)) {
        return true;
    }

    if (!allowAutoStart) {
        return false;
    }

    if (err.find("control listener connect failed") == std::string::npos) {
        return false;
    }

    std::string launchErr;
    if (!launchWorkerProcess(encoderOneBased, launchErr)) {
        err += "; worker auto-start failed: " + launchErr;
        return false;
    }

    autoStarted = true;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::string retryErr;
        if (sendWorkerControlCommand(encoderOneBased, command, retryErr)) {
            err.clear();
            return true;
        }
        err = retryErr;
    }

    err = "worker auto-started but control listener did not become reachable in time";
    return false;
}

static std::string summarizeInputConfig(const simplejson::Object& o, const std::string& prefix) {
    std::string inputType = o.getString(prefix + "InputType", o.getString("inputType", "rtp"));
    std::string rtpAddress = o.getString(prefix + "RtpAddress", o.getString("rtpAddress", ""));
    int rtpPort = o.getInt(prefix + "RtpPort", o.getInt("rtpPort", 5004));
    std::string iface = o.getString(prefix + "RtpInterface", o.getString("rtpInterface", ""));
    std::string audioDevice = o.getString(prefix + "AudioDevice", o.getString("audioDevice", ""));
    int sampleRate = o.getInt(prefix + "SampleRate", o.getInt("sampleRate", 48000));
    double gainDb = o.getDouble(prefix + "GainDb", o.getDouble("rtpGain", 0.0));

    std::ostringstream ss;
    if (inputType == "audio") {
        ss << "Audio Device: " << (audioDevice.empty() ? "Default" : audioDevice)
           << " @ " << sampleRate << " Hz, Gain " << gainDb << " dB";
    } else if (inputType == "srt") {
        std::string host = o.getString("srtHost", "");
        int port = o.getInt("srtPort", 0);
        ss << "SRT Input: " << (host.empty() ? "(host unset)" : host) << ":" << port
           << ", Gain " << gainDb << " dB";
    } else {
        std::string tag = (inputType == "axia") ? "Axia/RTP" : "RTP";
        ss << tag << ": " << (rtpAddress.empty() ? "(address unset)" : rtpAddress)
           << ":" << rtpPort
           << ", IF=" << (iface.empty() ? "Auto" : iface)
           << ", Gain " << gainDb << " dB";
    }
    return ss.str();
}

static std::string uiAssetPath(const std::string& fileName) {
    std::vector<std::string> roots;
    if (const char* envRoot = std::getenv("MULTICODER_UI_ROOT")) {
        roots.emplace_back(envRoot);
    }
    if (const char* envRoot = std::getenv("MC_UI_ROOT")) {
        roots.emplace_back(envRoot);
    }
    roots.emplace_back("/opt/multicoder/www");
    roots.emplace_back("www");
    roots.emplace_back("../www");
#ifdef MULTICODER_SOURCE_ROOT
    roots.emplace_back(std::string(MULTICODER_SOURCE_ROOT) + "/www");
#endif

    for (const auto& root : roots) {
        fs::path p = fs::path(root) / fileName;
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) return p.string();
    }
    return "";
}

static std::string encoderJsonNoLock(int i) {
    auto& e = g_encoders[i];
    simplejson::Object runtime = readRuntimeState(i + 1);
    int hbEpoch = runtime.getInt("workerHeartbeatEpoch", 0);
    int nowEpoch = static_cast<int>(std::time(nullptr));
    bool workerFresh = hbEpoch > 0 && (nowEpoch - hbEpoch) <= 10;

    bool ctl = workerFresh && runtime.getBool("controlListenerRunning", false);
    bool meta = workerFresh && runtime.getBool("metadataListenerRunning", false);

    bool aac = workerFresh ? runtime.getBool("workerAacRunning", false) : false;
    bool mp3 = workerFresh ? runtime.getBool("workerMp3Running", false) : false;
    bool hls = workerFresh ? runtime.getBool("workerHlsRunning", false) : false;
    bool srt = workerFresh ? runtime.getBool("workerSrtRunning", false) : false;

    // Keep in-memory cache aligned with worker health so subsequent logic uses consistent state.
    e.aac_running = aac;
    e.mp3_running = mp3;
    e.hls_running = hls;
    e.srt_running = srt;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"name\":\"Encoder-%d\",\"aac\":%s,\"mp3\":%s,\"hls\":%s,\"srt\":%s,\"controlListenerRunning\":%s,\"metadataListenerRunning\":%s}",
             i + 1, i + 1,
             aac ? "true" : "false",
             mp3 ? "true" : "false",
             hls ? "true" : "false",
             srt ? "true" : "false",
             ctl ? "true" : "false",
             meta ? "true" : "false");
    return buf;
}

static std::string encoderJson(int i) {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    return encoderJsonNoLock(i);
}

static int activeEncoderCount() {
    std::string sys = readFile("/etc/MC/system.json");
    if (!sys.empty()) {
        simplejson::Object o;
        if (o.parse(sys)) {
            int n = o.getInt("encoderCount", 0);
            if (n >= 1 && n <= MAX_ENCODERS) return n;
        }
    }
    return MAX_ENCODERS;
}

static std::string allEncodersJson() {
    std::lock_guard<std::mutex> lk(g_stateMutex);
    int count = activeEncoderCount();
    std::string out = "[";
    for (int i = 0; i < count; ++i) {
        if (i) out += ",";
        out += encoderJsonNoLock(i);
    }
    out += "]";
    return out;
}

static std::string tailLog(int encoderIdx, int lines = 100) {
    std::string path = "/etc/encoder" + std::to_string(encoderIdx + 1) +
                       "/logs/Encoder" + std::to_string(encoderIdx + 1) + ".log";
    std::ifstream f(path);
    if (!f.is_open()) {
        return "[No log file - encoder " + std::to_string(encoderIdx + 1) + " not yet started]\n";
    }
    std::vector<std::string> all;
    std::string line;
    while (std::getline(f, line)) all.push_back(line);
    int start = static_cast<int>(all.size()) - lines;
    if (start < 0) start = 0;
    std::ostringstream ss;
    for (int i = start; i < static_cast<int>(all.size()); ++i) ss << all[i] << "\n";
    return ss.str();
}

static std::string tailSysLog(int lines = 100) {
    std::ifstream f("/etc/MC/EncoderSys.log");
    if (!f.is_open()) return "[No system log yet]\n";
    std::vector<std::string> all;
    std::string line;
    while (std::getline(f, line)) all.push_back(line);
    int start = static_cast<int>(all.size()) - lines;
    if (start < 0) start = 0;
    std::ostringstream ss;
    for (int i = start; i < static_cast<int>(all.size()); ++i) ss << all[i] << "\n";
    return ss.str();
}

static std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

#ifdef _WIN32
static std::string wideToUtf8(const wchar_t* w) {
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    return out;
}
#endif

// Read stored interface preferences: { "interfaces": [ {"name","friendlyName","enabled"} ] }
static simplejson::Object readInterfacePrefs() {
    std::string raw = readFile("/etc/MC/interfaces.json");
    simplejson::Object o;
    if (!raw.empty()) o.parse(raw);
    return o;
}

static std::string interfacesJson() {
    simplejson::Object prefs = readInterfacePrefs();
    // Build a lookup map: adapterName -> {friendlyName, enabled}
    struct IfacePref { std::string friendlyName; bool enabled; };
    std::map<std::string, IfacePref> prefMap;
    // The prefs object stores the array as a serialised string — iterate manually.
    // We stored it as { "interfaces": [ ... ] }; SimpleJson stores arrays as children.
    // Walk prefs by serialising and re-parsing into a flat list.
    {
        std::string raw = readFile("/etc/MC/interfaces.json");
        // Simple line scan for stored prefs (avoid dependency on array parsing)
        // Format: [{"name":"X","friendlyName":"Y","enabled":true/false}, ...]
        // We look for "name":"...", "friendlyName":"...", "enabled":... triples.
        std::string pName, pFriendly;
        bool pEnabled = true;
        size_t pos = 0;
        auto skipWs = [&]() { while (pos < raw.size() && std::isspace((unsigned char)raw[pos])) ++pos; };
        auto readStr = [&]() -> std::string {
            if (pos >= raw.size() || raw[pos] != '"') return "";
            ++pos;
            std::string out;
            while (pos < raw.size() && raw[pos] != '"') {
                if (raw[pos] == '\\' && pos+1 < raw.size()) { ++pos; out += raw[pos++]; }
                else out += raw[pos++];
            }
            if (pos < raw.size()) ++pos;
            return out;
        };
        while (pos < raw.size()) {
            skipWs();
            if (pos >= raw.size()) break;
            if (raw[pos] == '{') { ++pos; pName.clear(); pFriendly.clear(); pEnabled = true; continue; }
            if (raw[pos] == '}') {
                ++pos;
                if (!pName.empty()) prefMap[pName] = { pFriendly.empty() ? pName : pFriendly, pEnabled };
                continue;
            }
            if (raw[pos] == '"') {
                std::string key = readStr();
                skipWs();
                if (pos < raw.size() && raw[pos] == ':') ++pos;
                skipWs();
                if (raw[pos] == '"') {
                    std::string val = readStr();
                    if (key == "name") pName = val;
                    else if (key == "friendlyName") pFriendly = val;
                } else {
                    // boolean/number
                    std::string tok;
                    while (pos < raw.size() && raw[pos] != ',' && raw[pos] != '}') tok += raw[pos++];
                    if (key == "enabled") pEnabled = (tok.find("true") != std::string::npos);
                }
            } else {
                ++pos;
            }
        }
    }

    std::ostringstream ss;
    ss << "[";
    bool first = true;
#ifdef _WIN32
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_UNSPEC;
    ULONG size = 15000;
    std::vector<unsigned char> buffer(size);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG rc = GetAdaptersAddresses(family, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        rc = GetAdaptersAddresses(family, flags, nullptr, addrs, &size);
    }
    if (rc == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES p = addrs; p; p = p->Next) {
            if (!p->AdapterName) continue;
            std::string name = p->AdapterName;
            std::string osFriendly = wideToUtf8(p->FriendlyName);
            // Merge with stored prefs
            auto it = prefMap.find(name);
            std::string friendly = (it != prefMap.end() && !it->second.friendlyName.empty())
                                   ? it->second.friendlyName : (osFriendly.empty() ? name : osFriendly);
            bool enabled = (it != prefMap.end()) ? it->second.enabled : true;
            if (!first) ss << ",";
            first = false;
            ss << "{\"name\":\"" << jsonEscape(name)
               << "\",\"friendlyName\":\"" << jsonEscape(friendly)
               << "\",\"enabled\":" << (enabled ? "true" : "false") << "}";
        }
    }
#else
    try {
        for (const auto& e : fs::directory_iterator("/sys/class/net")) {
            std::string n = e.path().filename().string();
            auto it = prefMap.find(n);
            std::string friendly = (it != prefMap.end() && !it->second.friendlyName.empty()) ? it->second.friendlyName : n;
            bool enabled = (it != prefMap.end()) ? it->second.enabled : true;
            if (!first) ss << ",";
            first = false;
            ss << "{\"name\":\"" << jsonEscape(n)
               << "\",\"friendlyName\":\"" << jsonEscape(friendly)
               << "\",\"enabled\":" << (enabled ? "true" : "false") << "}";
        }
    } catch (...) {
    }
#endif
    ss << "]";
    return ss.str();
}

static std::string audioInputsJson() {
    std::ostringstream ss;
    ss << "[";
    bool first = true;
#ifdef _WIN32
    UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSA caps{};
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        if (!first) ss << ",";
        first = false;
        // Use the device name as the stable ID (waveIn numeric indices change across reboots)
        ss << "{\"id\":\"" << jsonEscape(caps.szPname) << "\",\"name\":\""
           << jsonEscape(caps.szPname) << "\",\"friendlyName\":\""
           << jsonEscape(caps.szPname) << "\"}";
    }
#else
    std::ifstream f("/proc/asound/cards");
    std::string line;
    while (std::getline(f, line)) {
        size_t b1 = line.find('[');
        size_t b2 = line.find(']');
        size_t col = line.find(':');
        if (b1 == std::string::npos || b2 == std::string::npos || col == std::string::npos || b2 <= b1) continue;

        size_t cardStart = line.find_first_of("0123456789");
        if (cardStart == std::string::npos) continue;
        size_t cardEnd = cardStart;
        while (cardEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[cardEnd]))) ++cardEnd;
        std::string cardNum = line.substr(cardStart, cardEnd - cardStart);

        std::string name = line.substr(col + 1);
        while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) name.erase(name.begin());
        std::string id = "hw:" + cardNum;

        if (!first) ss << ",";
        first = false;
        ss << "{\"id\":\"" << jsonEscape(id) << "\",\"name\":\""
           << jsonEscape(name.empty() ? id : name) << "\",\"friendlyName\":\""
           << jsonEscape(name.empty() ? id : name) << "\"}";
    }
#endif
    ss << "]";
    return ss.str();
}

static std::string httpResp(int code, const std::string& ct, const std::string& body) {
    const char* status = (code == 200) ? "OK" : (code == 204) ? "No Content" : (code == 400) ? "Bad Request" : (code == 401) ? "Unauthorized" : (code == 404) ? "Not Found" : "Internal Server Error";
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\nConnection: close\r\n\r\n",
             code, status, ct.c_str(), body.size());
    return std::string(hdr) + body;
}

static std::string corsPreHdr() {
    return "HTTP/1.1 204 No Content\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type\r\n"
           "Content-Length: 0\r\nConnection: close\r\n\r\n";
}

static std::string parseMethod(const std::string& req) {
    std::istringstream is(req);
    std::string method;
    is >> method;
    return method;
}

static std::string parsePath(const std::string& req) {
    std::istringstream is(req);
    std::string method, fullPath;
    is >> method >> fullPath;
    size_t q = fullPath.find('?');
    return q == std::string::npos ? fullPath : fullPath.substr(0, q);
}

static std::string parseBody(const std::string& raw) {
    size_t sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos) return "";
    return raw.substr(sep + 4);
}

static std::string handleReq(const std::string& raw) {
    const std::string method = parseMethod(raw);
    const std::string path = parsePath(raw);
    const std::string body = parseBody(raw);

    if (path.find("..") != std::string::npos) return httpResp(400, "text/plain", "Bad Request");
    if (method == "OPTIONS") return corsPreHdr();

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::string html = readFile(uiAssetPath("index.html"));
        if (html.empty()) html = "<h1>MultiCoder - UI not installed</h1>";
        return httpResp(200, "text/html; charset=utf-8", html);
    }
    if (method == "GET" && path == "/styles.css") {
        std::string css = readFile(uiAssetPath("styles.css"));
        return css.empty() ? httpResp(404, "text/plain", "styles.css not found") : httpResp(200, "text/css", css);
    }
    if (method == "GET" && path == "/app.js") {
        std::string js = readFile(uiAssetPath("app.js"));
        return js.empty() ? httpResp(404, "text/plain", "app.js not found") : httpResp(200, "application/javascript", js);
    }
    if (method == "GET" && path == "/health") return httpResp(200, "application/json", "{\"status\":\"ok\",\"service\":\"multicoder-supervisor\"}");

    if (method == "GET" && path == "/api/encoders") return httpResp(200, "application/json", allEncodersJson());
    if (method == "GET" && path == "/api/syslog") return httpResp(200, "text/plain", tailSysLog());

    if (method == "POST" && path == "/api/admin/login") {
        std::string sys = readFile("/etc/MC/system.json");
        if (sys.empty()) return httpResp(200, "application/json", "{\"ok\":true,\"firstLoginRequired\":true}");
        simplejson::Object req;
        req.parse(body);
        simplejson::Object cfg;
        cfg.parse(sys);
        std::string reqUser = req.getString("user", "");
        std::string reqPass = req.getString("pass", "");
        std::string cfgUser = cfg.getString("adminUser", "Admin");
        std::string cfgPass = cfg.getString("adminPass", "change-me");
        bool firstLoginRequired = cfg.getBool("firstLoginRequired", true);
        if (reqUser == cfgUser && reqPass == cfgPass) {
            std::string resp = std::string("{\"ok\":true,\"firstLoginRequired\":") + (firstLoginRequired ? "true" : "false") + "}";
            return httpResp(200, "application/json", resp);
        }
        return httpResp(401, "application/json", "{\"ok\":false}");
    }

    if (method == "GET" && path == "/api/admin/interfaces") return httpResp(200, "application/json", interfacesJson());
    if (method == "POST" && path == "/api/admin/interfaces") {
        fs::create_directories("/etc/MC");
        std::ofstream f("/etc/MC/interfaces.json");
        f << body;
        return httpResp(200, "application/json", "{\"saved\":true}");
    }
    if (method == "GET" && path == "/api/admin/audio-inputs") return httpResp(200, "application/json", audioInputsJson());

    if (method == "GET" && path == "/api/admin/config") {
        std::string cfg = readFile("/etc/MC/system.json");
        if (cfg.empty()) cfg = "{}";
        return httpResp(200, "application/json", cfg);
    }
    if (method == "POST" && path == "/api/admin/config") {
        fs::create_directories("/etc/MC");
        std::ofstream f("/etc/MC/system.json");
        f << body;
        return httpResp(200, "application/json", "{\"saved\":true}");
    }

    const std::string prefix = "/api/encoder/";

    // HLS segment/playlist serving: GET /encoder/{N}/hls/{filename}
    // Serves files written by the worker's FFmpeg HLS encoder from /etc/encoder{N}/hls/
    const std::string hlsPrefix = "/encoder/";
    if (method == "GET" && path.rfind(hlsPrefix, 0) == 0) {
        std::string rest = path.substr(hlsPrefix.size());
        size_t s1 = rest.find('/');
        if (s1 != std::string::npos && rest.substr(s1 + 1, 4) == "hls/") {
            std::string idStr    = rest.substr(0, s1);
            std::string filename = rest.substr(s1 + 5); // skip "hls/"
            int encIdx = atoi(idStr.c_str());
            // Validate index and filename.
            // Allow:
            //   - index.m3u8
            //   - segment-*.aac / seg_*.ts (legacy flat layout)
            //   - segments/segment-*.aac (original encoder layout)
            bool badName = filename.empty()
                        || filename.find('\\') != std::string::npos
                        || filename.find("..") != std::string::npos;
            size_t slashPos = filename.find('/');
            if (!badName && slashPos != std::string::npos) {
                bool allowedSegPath = filename.rfind("segments/", 0) == 0 &&
                                      filename.find('/', 9) == std::string::npos;
                if (!allowedSegPath) badName = true;
            }
            if (!badName && encIdx >= 1 && encIdx <= MAX_ENCODERS) {
                std::string data;
                std::string proxyErr;
                bool proxied = proxyHlsFromWorker(encIdx, filename, data, proxyErr);

                // Prefer worker proxy so we serve the transformed/timed playlist.
                // Fallback to direct file read when worker playback proxy is unavailable.
                if (!proxied) {
                    std::string filePath = "/etc/encoder" + std::to_string(encIdx) + "/hls/" + filename;
                    data = readFile(filePath);
                    if (data.empty()) {
                        return httpResp(404, "text/plain", "HLS file not found");
                    }
                }

                bool isM3u8 = filename.size() >= 5 &&
                              filename.compare(filename.size() - 5, 5, ".m3u8") == 0;
                bool isAac = filename.size() >= 4 &&
                             filename.compare(filename.size() - 4, 4, ".aac") == 0;

                // If we are serving a fallback playlist from disk (not proxied), normalize it
                // to timed HLS format expected by Orban devices.
                if (isM3u8 && !proxied) {
                    simplejson::Object hlsCfg = readJsonFile("/etc/encoder" + std::to_string(encIdx) + "/hls.json");
                    int segSecs = hlsCfg.getInt("segmentSeconds", 6);
                    if (segSecs <= 0) segSecs = 6;
                    int startOffset = hlsCfg.getInt("startTimeOffset", 0);

                    simplejson::Object metaRt = readJsonFile("/etc/encoder" + std::to_string(encIdx) + "/metadata_runtime.json");
                    std::string metaPayload = metaRt.getString("lastFormattedHLS", "");
                    for (char& c : metaPayload) {
                        if (c == '\r' || c == '\n') c = ' ';
                    }

                    std::vector<std::string> lines;
                    {
                        std::istringstream iss(data);
                        std::string ln;
                        while (std::getline(iss, ln)) {
                            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                            lines.push_back(ln);
                        }
                    }

                    int mediaSeq = -1;
                    std::vector<std::string> segUris;
                    std::vector<int> segNums;
                    for (const auto& ln : lines) {
                        if (ln.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
                            try { mediaSeq = std::stoi(ln.substr(22)); } catch (...) { mediaSeq = -1; }
                            continue;
                        }
                        if (ln.empty() || ln[0] == '#') continue;
                        if (ln.find(".aac") == std::string::npos) continue;
                        segUris.push_back(ln);

                        int seq = -1;
                        size_t p = ln.rfind("segment-");
                        if (p != std::string::npos) {
                            p += 8;
                            size_t q = p;
                            while (q < ln.size() && std::isdigit(static_cast<unsigned char>(ln[q]))) q++;
                            if (q > p) {
                                try { seq = std::stoi(ln.substr(p, q - p)); } catch (...) { seq = -1; }
                            }
                        }
                        segNums.push_back(seq);
                    }

                    if (!segUris.empty()) {
                        if (mediaSeq < 0) {
                            mediaSeq = (segNums[0] >= 0) ? segNums[0] : 0;
                        }

                        static std::mutex s_pdtMutex;
                        static std::map<int, int> s_anchorSeqByEncoder;
                        static std::map<int, double> s_anchorEpochByEncoder;

                        double firstEpoch = 0.0;
                        {
                            std::lock_guard<std::mutex> lk(s_pdtMutex);
                            int firstSeq = (segNums[0] >= 0) ? segNums[0] : mediaSeq;
                            auto itSeq = s_anchorSeqByEncoder.find(encIdx);
                            auto itEp = s_anchorEpochByEncoder.find(encIdx);
                            if (itSeq == s_anchorSeqByEncoder.end() || itEp == s_anchorEpochByEncoder.end() ||
                                firstSeq < itSeq->second - 50) {
                                double nowEpoch = std::chrono::duration<double>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                s_anchorSeqByEncoder[encIdx] = firstSeq;
                                s_anchorEpochByEncoder[encIdx] = nowEpoch - 0.5 * static_cast<double>(segSecs);
                            } else if (firstSeq > itSeq->second) {
                                s_anchorEpochByEncoder[encIdx] +=
                                    static_cast<double>(firstSeq - itSeq->second) * static_cast<double>(segSecs);
                                s_anchorSeqByEncoder[encIdx] = firstSeq;
                            }
                            firstEpoch = s_anchorEpochByEncoder[encIdx];
                        }

                        auto epochToUtcIso = [](double sec) {
                            auto tp = std::chrono::system_clock::time_point(
                                std::chrono::milliseconds(static_cast<long long>(sec * 1000.0)));
                            auto tt = std::chrono::system_clock::to_time_t(tp);
                            std::tm tmUtc{};
#ifdef _WIN32
                            gmtime_s(&tmUtc, &tt);
#else
                            gmtime_r(&tt, &tmUtc);
#endif
                            char buf[64];
                            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmUtc);
                            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
                            char out[96];
                            snprintf(out, sizeof(out), "%s.%03lldZ", buf, static_cast<long long>(ms));
                            return std::string(out);
                        };

                        std::ostringstream out;
                        out << "#EXTM3U\n";
                        out << "#EXT-X-VERSION:6\n";
                        out << "#EXT-X-TARGETDURATION:" << segSecs << "\n";
                        out << "#EXT-X-MEDIA-SEQUENCE:" << mediaSeq << "\n";
                        out << "#EXT-X-INDEPENDENT-SEGMENTS\n";
                        out << "#EXT-X-START:TIME-OFFSET=" << startOffset << ",PRECISE=YES\n";

                        double curEpoch = firstEpoch;
                        for (size_t i = 0; i < segUris.size(); ++i) {
                            out << "#EXT-X-PROGRAM-DATE-TIME:" << epochToUtcIso(curEpoch) << "\n";
                            out << "#EXTINF:" << std::fixed << std::setprecision(3)
                                << static_cast<double>(segSecs) << ",";
                            if (!metaPayload.empty()) out << metaPayload;
                            out << "\n";
                            out << segUris[i] << "\n";
                            curEpoch += static_cast<double>(segSecs);
                        }
                        data = out.str();
                    }
                }

                std::string ct = isM3u8 ? "audio/mpegurl" : (isAac ? "audio/aac" : "video/mp2t");
                // For M3U8 playlist: allow re-fetch so clients always get the latest segment list.
                // Build response manually with explicit cache headers.
                const char* cacheHdr = isM3u8
                    ? "Cache-Control: no-cache, no-store\r\nPragma: no-cache\r\n"
                    : "Cache-Control: max-age=600\r\n";
                char hdr[768];
                snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                         "Access-Control-Allow-Origin: *\r\n%sConnection: close\r\n\r\n",
                         ct.c_str(), data.size(), cacheHdr);
                return std::string(hdr) + data;
            }
        }
    }

    if (path.rfind(prefix, 0) == 0) {
        std::string rest = path.substr(prefix.size());
        size_t slash1 = rest.find('/');
        std::string idStr = slash1 == std::string::npos ? rest : rest.substr(0, slash1);
        std::string sub = slash1 == std::string::npos ? "" : rest.substr(slash1 + 1);

        int idx = atoi(idStr.c_str()) - 1;
        if (idx < 0 || idx >= MAX_ENCODERS) return httpResp(404, "application/json", "{\"error\":\"encoder not found\"}");

        // Per-encoder admin config (listen links, HLS playback port, etc.)
        if (method == "GET" && sub == "admin-config") {
            std::string cfgPath = "/etc/encoder" + std::to_string(idx + 1) + "/encoder_admin.json";
            std::string raw = readFile(cfgPath);
            if (raw.empty()) raw = "{}";
            return httpResp(200, "application/json", raw);
        }
        if (method == "POST" && sub == "admin-config") {
            std::string dir = "/etc/encoder" + std::to_string(idx + 1);
            fs::create_directories(dir);
            std::ofstream f(dir + "/encoder_admin.json");
            f << body;
            return httpResp(200, "application/json", "{\"saved\":true}");
        }

        // UI control listeners: acknowledge command without restarting whole app.
        if (method == "POST" && (sub == "control/start" || sub == "control/stop")) {
            bool start = (sub == "control/start");
            simplejson::Object runtime = readRuntimeState(idx + 1);
            bool metaState = runtime.getBool("metadataListenerRunning", false);
            writeRuntimeState(idx + 1, start, metaState);
            int ctlPort = controlPortForEncoder(idx + 1);
            if (start) {
                appendEncoderLog(idx + 1, "Control listener start requested on port " + std::to_string(ctlPort));
            } else {
                appendEncoderLog(idx + 1, "Control listener stop requested");
            }
            std::string bodyResp = std::string("{\"ok\":true,\"listener\":\"control\",\"state\":\"") +
                                   (start ? "running" : "stopped") + "\"}";
            return httpResp(200, "application/json", bodyResp);
        }

        // Input connect/disconnect state used by UI and worker runtime.
        if (method == "POST" && (sub == "input/connect" || sub == "input/disconnect")) {
            bool connected = (sub == "input/connect");
            if (connected) {
                simplejson::Object req;
                bool parseOk = req.parse(body);
                appendEncoderLog(idx + 1, "Input connect request: body size=" + std::to_string(body.size()) + 
                                 " parseOk=" + (parseOk ? "true" : "false"));
                
                simplejson::Object cfg = readJsonFile("/etc/encoder" + std::to_string(idx + 1) + "/input.json");
                simplejson::Object rt = readRuntimeState(idx + 1);

                std::string inputType = req.getString("inputType", cfg.getString("inputType", "rtp"));
                std::string rtpAddress = req.getString("rtpAddress", cfg.getString("rtpAddress", ""));
                int rtpPort = req.getInt("rtpPort", cfg.getInt("rtpPort", 5004));
                std::string iface = req.getString("rtpInterface", cfg.getString("rtpInterface", ""));
                std::string audioDevice = req.getString("audioDevice", cfg.getString("audioDevice", ""));
                int sampleRate = req.getInt("sampleRate", cfg.getInt("sampleRate", 48000));
                double gainDb = req.getDouble("rtpGain", cfg.getDouble("rtpGain", 0.0));

                // Validate parameters
                std::string validationError;
                if (inputType == "rtp" || inputType == "axia") {
                    if (rtpAddress.empty()) {
                        validationError = "RTP/Axia input: multicast address is required";
                    } else if (rtpPort <= 0 || rtpPort > 65535) {
                        validationError = "RTP/Axia input: port must be 1-65535, got " + std::to_string(rtpPort);
                    }
                } else if (inputType != "audio" && inputType != "srt") {
                    validationError = "Unknown input type: " + inputType;
                }

                if (!validationError.empty()) {
                    appendEncoderLog(idx + 1, "Input connect FAILED: " + validationError);
                    return httpResp(400, "application/json", 
                        "{\"ok\":false,\"error\":\"" + jsonEscape(validationError) + "\"}");
                }

                rt.setBool("inputConnected", true);
                rt.setString("sessionInputType", inputType);
                rt.setString("sessionRtpAddress", rtpAddress);
                rt.setInt("sessionRtpPort", rtpPort);
                rt.setString("sessionRtpInterface", iface);
                rt.setString("sessionAudioDevice", audioDevice);
                rt.setInt("sessionSampleRate", sampleRate);
                rt.setRaw("sessionGainDb", std::to_string(gainDb));

                appendEncoderLog(idx + 1, "Input connect confirmed: type=" + inputType + 
                                 (inputType != "audio" ? " addr=" + rtpAddress + ":" + std::to_string(rtpPort) +
                                  (iface.empty() ? "" : " iface=" + iface) : " device=" + (audioDevice.empty() ? "default" : audioDevice)) +
                                 " gain=" + std::to_string(gainDb) + "dB");

                std::string dir = "/etc/encoder" + std::to_string(idx + 1);
                fs::create_directories(dir);
                atomicWriteRuntimeState(idx + 1, rt);

                // For SRT input, tell the worker to start its SRT relay immediately
                // so the SRT port is bound/connected as soon as the user clicks Connect.
                if (inputType == "srt") {
                    std::string srtCmdErr;
                    sendWorkerControlCommand(idx + 1, "StartSRTInput", srtCmdErr);
                    // Non-fatal if the worker isn't up yet — it will read srtRelayActive on start.
                }

                // Auto-start the worker if it is not currently running.
                // Use both heartbeat age AND a recent-launch marker to prevent
                // zombie spawning when the user clicks Connect rapidly.
                int hbEpoch     = rt.getInt("workerHeartbeatEpoch", 0);
                int launchEpoch = rt.getInt("workerLaunchEpoch", 0);
                int nowEpoch    = static_cast<int>(std::time(nullptr));
                bool heartbeatFresh    = hbEpoch     > 0 && (nowEpoch - hbEpoch)     <= 8;
                bool recentlyLaunched  = launchEpoch > 0 && (nowEpoch - launchEpoch) <= 25;
                bool workerFresh = heartbeatFresh || recentlyLaunched;
                if (!workerFresh) {
                    // Stamp the launch epoch BEFORE spawning so that rapid clicks
                    // see the marker and skip launching a second process.
                    rt.setInt("workerLaunchEpoch", nowEpoch);
                    atomicWriteRuntimeState(idx + 1, rt);
                    std::string launchErr;
                    if (launchWorkerProcess(idx + 1, launchErr)) {
                        appendEncoderLog(idx + 1, "Worker auto-started on input/connect");
                        appendSysLog(idx + 1, "Worker auto-started on input/connect");
                    } else {
                        appendEncoderLog(idx + 1, "Worker auto-start failed on input/connect: " + launchErr);
                        appendSysLog(idx + 1, "Worker auto-start failed on input/connect: " + launchErr);
                    }
                }
            } else {
                // Stop the SRT relay if one was active before disconnecting
                simplejson::Object rtDis = readRuntimeState(idx + 1);
                std::string prevType = rtDis.getString("sessionInputType", "");
                if (prevType == "srt") {
                    std::string srtErr;
                    sendWorkerControlCommand(idx + 1, "StopSRTInput", srtErr);
                    // Also clear relay state so VU meter stops polling
                    rtDis.setBool("srtRelayActive", false);
                    rtDis.setInt ("srtRelayPort",   0);
                    rtDis.setString("srtRelayAddr",  "");
                    atomicWriteRuntimeState(idx + 1, rtDis);
                }
                setRuntimeInputConnected(idx + 1, false);
            }
            std::string bodyResp = std::string("{\"ok\":true,\"inputConnected\":") +
                                   (connected ? "true" : "false") + "}";
            return httpResp(200, "application/json", bodyResp);
        }

        if (method == "POST" && sub == "input/preview-gain") {
            simplejson::Object req;
            req.parse(body);
            double gainDb = req.getDouble("gainDb", 0.0);
            simplejson::Object rt = readRuntimeState(idx + 1);
            rt.setRaw("sessionGainDb", std::to_string(gainDb));
            atomicWriteRuntimeState(idx + 1, rt);
            return httpResp(200, "application/json", "{\"ok\":true}");
        }

        if (method == "GET" && sub == "input/levels") {
            simplejson::Object runtime = readRuntimeState(idx + 1);
            bool connected = runtime.getBool("inputConnected", false);
            // Real levels are expected from worker telemetry; return silence defaults until available.
            int left = runtime.getInt("inputLevelL", -60);
            int right = runtime.getInt("inputLevelR", -60);
            if (!connected) {
                left = -60;
                right = -60;
            }
            std::string bodyResp = std::string("{\"connected\":") + (connected ? "true" : "false") +
                                   ",\"leftDb\":" + std::to_string(left) +
                                   ",\"rightDb\":" + std::to_string(right) + "}";
            return httpResp(200, "application/json", bodyResp);
        }

        if (method == "GET" && sub == "input/status") {
            simplejson::Object runtime = readRuntimeState(idx + 1);
            simplejson::Object saved = readJsonFile("/etc/encoder" + std::to_string(idx + 1) + "/input.json");
            bool connected = runtime.getBool("inputConnected", false);

            std::string active = connected ? summarizeInputConfig(runtime, "session") : "Disconnected (no active session input)";
            std::string savedSummary = summarizeInputConfig(saved, "");

            std::string bodyResp = std::string("{\"connected\":") + (connected ? "true" : "false") +
                                   ",\"activeSessionInput\":\"" + jsonEscape(active) + "\"" +
                                   ",\"savedProfileInput\":\"" + jsonEscape(savedSummary) + "\"}"
                                   ;
            return httpResp(200, "application/json", bodyResp);
        }

        // Metadata ingestion mode control: exactly one mode active at a time.
        if (method == "POST" && (sub == "metadata/start" || sub == "metadata/stop")) {
            bool start = (sub == "metadata/start");
            const int enc = idx + 1;
            const std::string metaPath = "/etc/encoder" + std::to_string(enc) + "/metadata.json";
            simplejson::Object metaCfg = readJsonFile(metaPath);
            simplejson::Object req;
            req.parse(body);

            std::string mode = req.getString("mode", metaCfg.getString("mode", "listen"));
            for (char& c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (mode != "listen" && mode != "pull") mode = "listen";

            int listenPort = req.getInt("listenPort", metaCfg.getInt("listenPort", 9000 + idx * 10));
            std::string pullHost = req.getString("dataConnectHost", metaCfg.getString("dataConnectHost", ""));
            int pullPort = req.getInt("dataConnectPort", metaCfg.getInt("dataConnectPort", 0));

            if (start) {
                std::string validationError;
                if (mode == "listen") {
                    if (listenPort <= 0 || listenPort > 65535) {
                        validationError = "Listen mode requires a valid listen port (1-65535)";
                    }
                } else {
                    if (pullHost.empty()) {
                        validationError = "Pull mode requires Data Connect Host/IP";
                    } else if (pullPort <= 0 || pullPort > 65535) {
                        validationError = "Pull mode requires Data Connect Port (1-65535)";
                    }
                }
                if (!validationError.empty()) {
                    appendEncoderLog(enc, "Metadata start FAILED: " + validationError);
                    appendSysLog(enc, "Metadata start FAILED: " + validationError);
                    return httpResp(400, "application/json",
                        "{\"ok\":false,\"error\":\"" + jsonEscape(validationError) + "\"}");
                }
            }

            metaCfg.setString("mode", mode);
            metaCfg.setInt("listenPort", listenPort);
            metaCfg.setString("dataConnectHost", pullHost);
            metaCfg.setInt("dataConnectPort", pullPort);
            {
                std::ofstream mf(metaPath, std::ios::trunc);
                mf << metaCfg.serialize();
            }

            simplejson::Object runtime = readRuntimeState(enc);
            bool ctlState = runtime.getBool("controlListenerRunning", false);
            writeRuntimeState(enc, ctlState, start);

            if (start) {
                if (mode == "listen") {
                    appendEncoderLog(enc, "Metadata start requested in LISTEN mode on port " + std::to_string(listenPort));
                    appendSysLog(enc, "Metadata start requested in LISTEN mode on port " + std::to_string(listenPort));
                } else {
                    appendEncoderLog(enc, "Metadata start requested in PULL mode from " + pullHost + ":" + std::to_string(pullPort));
                    appendSysLog(enc, "Metadata start requested in PULL mode from " + pullHost + ":" + std::to_string(pullPort));
                }
            } else {
                appendEncoderLog(enc, "Metadata stop requested");
                appendSysLog(enc, "Metadata stop requested");
            }

            std::string bodyResp = std::string("{\"ok\":true,\"listener\":\"metadata\",\"state\":\"") +
                                   (start ? "running" : "stopped") + "\",\"mode\":\"" + mode + "\"}";
            return httpResp(200, "application/json", bodyResp);
        }

        if (method == "POST" && sub == "metadata/connect") {
            // Explicit test-connect endpoint only. Start/Stop controls the actual runtime mode.
            simplejson::Object req;
            req.parse(body);
            std::string host = req.getString("host", "");
            int port = req.getInt("port", 0);
            appendEncoderLog(idx + 1, "Metadata test connect requested to " + host + ":" + std::to_string(port));
            appendSysLog(idx + 1, "Metadata test connect requested to " + host + ":" + std::to_string(port));

            std::string err;
            bool ok = testTcpConnect(host, port, err);
            if (ok) {
                appendEncoderLog(idx + 1, "Metadata test connect SUCCESS to " + host + ":" + std::to_string(port));
                appendSysLog(idx + 1, "Metadata test connect SUCCESS to " + host + ":" + std::to_string(port));
                return httpResp(200, "application/json", "{\"ok\":true,\"status\":\"success\"}");
            }

            appendEncoderLog(idx + 1, "Metadata test connect FAILED to " + host + ":" + std::to_string(port) + " (" + err + ")");
            appendSysLog(idx + 1, "Metadata test connect FAILED to " + host + ":" + std::to_string(port) + " (" + err + ")");
            return httpResp(200, "application/json", "{\"ok\":false,\"status\":\"failed\",\"error\":\"" + jsonEscape(err) + "\"}");
        }

        if (method == "GET" && sub == "metadata/status") {
            simplejson::Object runtime = readRuntimeState(idx + 1);
            simplejson::Object metaCfg = readJsonFile("/etc/encoder" + std::to_string(idx + 1) + "/metadata.json");
            simplejson::Object metaRt = readJsonFile("/etc/encoder" + std::to_string(idx + 1) + "/metadata_runtime.json");
            bool listenerRunning = runtime.getBool("metadataListenerRunning", false);
            std::string mode = metaCfg.getString("mode", "listen");
            int listenPort = metaCfg.getInt("listenPort", 9000 + idx * 10);
            std::string pullHost = metaCfg.getString("dataConnectHost", "");
            int pullPort = metaCfg.getInt("dataConnectPort", 0);
            int eventCount = metaRt.getInt("eventCount", 0);
            std::string lastPayloadUtc = metaRt.getString("lastPayloadUtc", "");
            std::string lastRawXml       = metaRt.getString("lastRawXml", "");
            std::string lastFmtAAC       = metaRt.getString("lastFormattedAAC", "");
            std::string lastFmtMP3       = metaRt.getString("lastFormattedMP3", "");
            std::string lastFmtHLS       = metaRt.getString("lastFormattedHLS", "");
            std::string lastFmtSRT       = metaRt.getString("lastFormattedSRT", "");
            std::string bodyResp = std::string("{\"listenerRunning\":") + (listenerRunning ? "true" : "false") +
                                   ",\"mode\":\"" + jsonEscape(mode) + "\"" +
                                   ",\"listenPort\":" + std::to_string(listenPort) +
                                   ",\"dataConnectHost\":\"" + jsonEscape(pullHost) + "\"" +
                                   ",\"dataConnectPort\":" + std::to_string(pullPort) +
                                   ",\"eventCount\":" + std::to_string(eventCount) +
                                   ",\"lastPayloadUtc\":\"" + jsonEscape(lastPayloadUtc) + "\"" +
                                   ",\"lastRawXml\":\"" + jsonEscape(lastRawXml) + "\"" +
                                   ",\"lastFormattedAAC\":\"" + jsonEscape(lastFmtAAC) + "\"" +
                                   ",\"lastFormattedMP3\":\"" + jsonEscape(lastFmtMP3) + "\"" +
                                   ",\"lastFormattedHLS\":\"" + jsonEscape(lastFmtHLS) + "\"" +
                                   ",\"lastFormattedSRT\":\"" + jsonEscape(lastFmtSRT) + "\"}";
            return httpResp(200, "application/json", bodyResp);
        }

        if (method == "GET" && sub.empty()) return httpResp(200, "application/json", encoderJson(idx));
        if (method == "GET" && sub == "log") return httpResp(200, "text/plain", tailLog(idx));

        if (method == "GET" && sub == "config") {
            std::string cfgDir = "/etc/encoder" + std::to_string(idx + 1) + "/";
            std::string aac = readFile(cfgDir + "aac.json"); if (aac.empty()) aac = "{}";
            std::string mp3 = readFile(cfgDir + "mp3.json"); if (mp3.empty()) mp3 = "{}";
            std::string hls = readFile(cfgDir + "hls.json"); if (hls.empty()) hls = "{}";
            std::string srt = readFile(cfgDir + "srt.json"); if (srt.empty()) srt = "{}";
            std::string inp = readFile(cfgDir + "input.json"); if (inp.empty()) inp = "{}";
            std::string ctl = readFile(cfgDir + "control.json"); if (ctl.empty()) ctl = "{}";
            std::string meta = readFile(cfgDir + "metadata.json"); if (meta.empty()) meta = "{}";
            std::string resp = "{\"input\":" + inp + ",\"control\":" + ctl + ",\"metadata\":" + meta + ",\"aac\":" + aac + ",\"mp3\":" + mp3 + ",\"hls\":" + hls + ",\"srt\":" + srt + "}";
            return httpResp(200, "application/json", resp);
        }

        if (method == "POST" && sub.rfind("config/", 0) == 0) {
            std::string section = sub.substr(7);
            static const std::vector<std::string> allowed = {"input", "control", "metadata", "aac", "mp3", "hls", "srt"};
            bool ok = false;
            for (const auto& s : allowed) if (s == section) { ok = true; break; }
            if (!ok) return httpResp(400, "application/json", "{\"error\":\"unknown section\"}");
            std::string cfgDir = "/etc/encoder" + std::to_string(idx + 1) + "/";
            fs::create_directories(cfgDir);
            // Open in binary mode so the file is never written with a BOM or
            // CR/LF translations regardless of platform text-mode defaults.
            std::ofstream f(cfgDir + section + ".json", std::ios::binary | std::ios::trunc);
            if (!f.is_open())
                return httpResp(500, "application/json", "{\"error\":\"failed to open config file for writing\"}");
            f << body;
            f.close();
            if (f.fail())
                return httpResp(500, "application/json", "{\"error\":\"config file write failed\"}");
            return httpResp(200, "application/json", "{\"saved\":true}");
        }

        if (method == "POST") {
            size_t slash2 = sub.find('/');
            std::string streamType = slash2 == std::string::npos ? sub : sub.substr(0, slash2);
            std::string action = slash2 == std::string::npos ? "" : sub.substr(slash2 + 1);
            if (action == "start" || action == "stop") {
                bool on = (action == "start");
                std::string cmd = streamCommandFor(streamType, on);
                if (cmd.empty()) return httpResp(400, "application/json", "{\"error\":\"unknown stream type\"}");

                std::string streamUpper = streamType;
                for (char& c : streamUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                appendEncoderLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " requested");
                appendSysLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " requested");

                std::string err;
                bool autoStarted = false;
                bool ok = sendWorkerControlWithAutoStart(idx + 1, cmd, on, autoStarted, err);
                if (!ok) {
                    {
                        std::lock_guard<std::mutex> lk(g_stateMutex);
                        // If control listener is unreachable, the worker is down or not accepting
                        // commands; clear stale LIVE badges to keep UI status conservative/accurate.
                        if (err.find("control listener connect failed") != std::string::npos) {
                            g_encoders[idx].aac_running = false;
                            g_encoders[idx].mp3_running = false;
                            g_encoders[idx].hls_running = false;
                            g_encoders[idx].srt_running = false;
                        } else {
                            if (streamType == "aac") g_encoders[idx].aac_running = false;
                            else if (streamType == "mp3") g_encoders[idx].mp3_running = false;
                            else if (streamType == "hls") g_encoders[idx].hls_running = false;
                            else if (streamType == "srt") g_encoders[idx].srt_running = false;
                        }
                    }
                    appendEncoderLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " FAILED: " + err);
                    appendSysLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " FAILED: " + err);
                    return httpResp(200, "application/json", "{\"ok\":false,\"error\":\"" + jsonEscape(err) + "\"}");
                }

                if (autoStarted) {
                    appendEncoderLog(idx + 1, "Worker auto-started to satisfy " + streamUpper + " " + (on ? "start" : "stop") + " request");
                    appendSysLog(idx + 1, "Worker auto-started to satisfy " + streamUpper + " " + (on ? "start" : "stop") + " request");
                }

                {
                    std::lock_guard<std::mutex> lk(g_stateMutex);
                    if (streamType == "aac") g_encoders[idx].aac_running = on;
                    else if (streamType == "mp3") g_encoders[idx].mp3_running = on;
                    else if (streamType == "hls") g_encoders[idx].hls_running = on;
                    else if (streamType == "srt") g_encoders[idx].srt_running = on;
                }
                appendEncoderLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " acknowledged by worker");
                appendSysLog(idx + 1, streamUpper + " " + (on ? "start" : "stop") + " acknowledged by worker");
                return httpResp(200, "application/json", encoderJson(idx));
            }
        }
    }

    return httpResp(404, "text/plain", "Not Found");
}

static void handleClient(int fd) {
    char buf[16384] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        std::string resp = handleReq(std::string(buf, n));
        send(fd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
    }
    close_socket(fd);
}

void start_supervisor_api(int port) {
    initSocketsOnce();
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << "\n";
        return;
    }
    int opt = 1;
#ifdef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() failed on port " << port << ": " << strerror(errno) << "\n";
        close_socket(sfd);
        return;
    }

    listen(sfd, 64);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    g_listenSocket = sfd;
    std::cout << "MultiCoder UI listening on http://0.0.0.0:" << port << "\n";

    std::thread([sfd]() {
        while (g_svAcceptRunning) {
            sockaddr_in cli{};
            socklen_t len = sizeof(cli);
            int cfd = accept(sfd, (sockaddr*)&cli, &len);
            if (cfd < 0) continue;
            std::thread(handleClient, cfd).detach();
        }
    }).detach();
}

void stop_supervisor_api() {
    g_svAcceptRunning = false;

    // Closing the listen socket unblocks the accept() call in the accept thread.
    if (g_listenSocket >= 0) {
        close_socket(g_listenSocket);
        g_listenSocket = -1;
    }

#ifdef _WIN32
    // Explicitly terminate every known worker process.  The Windows Job Object
    // (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) also handles the hard-kill case where
    // the supervisor is terminated by the OS before this function is reached.
    std::lock_guard<std::mutex> lk(g_workerHandlesMutex);
    for (auto& [idx, h] : g_workerHandles) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
    g_workerHandles.clear();
#endif
}
