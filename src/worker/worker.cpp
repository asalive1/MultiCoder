// worker.cpp — MultiCoder Worker Implementation
// Handles logging, control port listener, metadata listener, and stream lifecycle.
// Real encoding is deferred to the encoding pipeline modules (AAC, MP3, HLS, SRT).

#include "worker.h"
#include "SimpleJson.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <csignal>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <iomanip>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winreg.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Advapi32.lib")
typedef int ssize_t;
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace {
constexpr int kIcecastFailureTimeoutUs = 60 * 1000 * 1000;

std::string trimCopy(const std::string& value) {
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string maskSrtUriPassphrase(const std::string& uri) {
    const std::string key = "passphrase=";
    size_t pos = uri.find(key);
    if (pos == std::string::npos) return uri;
    size_t valueStart = pos + key.size();
    size_t valueEnd = uri.find('&', valueStart);
    if (valueEnd == std::string::npos) valueEnd = uri.size();
    std::string masked = uri;
    masked.replace(valueStart, valueEnd - valueStart, "***");
    return masked;
}

int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

bool isAacProfileAllowed(const std::string& profile) {
    static const std::set<std::string> allowed = {
        "aac_low", "aac_he", "aac_he_v2", "mpeg2_aac_low"
    };
    return allowed.find(profile) != allowed.end();
}
}

#ifdef _WIN32
static void close_socket(int fd) { closesocket((SOCKET)fd); }
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

static std::atomic<bool> g_workerShutdown{false};
static std::mutex g_runtimeStateFileMutex;

static void sigHandler(int) { g_workerShutdown = true; }

static std::string readTextFile(const std::string& path) {
#ifdef _WIN32
    // Resolve /etc/... style paths to absolute Windows paths before calling CreateFileW.
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
    // Strip UTF-8 BOM (0xEF 0xBB 0xBF) if present.
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
    std::string s = readTextFile(path);
    if (!s.empty()) o.parse(s);
    return o;
}

static simplejson::Object readRuntimeState(const std::string& cfgDir) {
    std::lock_guard<std::mutex> lk(g_runtimeStateFileMutex);
    return readJsonFile(cfgDir + "/runtime_state.json");
}

static std::string toWindowsPath(const std::string& p) {
#ifdef _WIN32
    char resolved[MAX_PATH] = {};
    if (_fullpath(resolved, p.c_str(), MAX_PATH)) return std::string(resolved);
    std::string r = p;
    for (char& c : r) if (c == '/') c = '\\';
    return r;
#else
    return p;
#endif
}

static void writeRuntimeState(const std::string& cfgDir, const simplejson::Object& state) {
    std::lock_guard<std::mutex> lk(g_runtimeStateFileMutex);
    // Atomic write: write to .wk.tmp then MoveFileExW so readers never see a truncated file.
    std::string path    = cfgDir + "/runtime_state.json";
    std::string tmpPath = cfgDir + "/runtime_state.json.wk.tmp";  // .wk.tmp = worker; supervisor uses .sv.tmp
    {
        std::ofstream f(tmpPath, std::ios::trunc);
        if (!f.is_open()) return;
        f << state.serialize();
    } // f closed/flushed here
#ifdef _WIN32
    std::string winTmp = toWindowsPath(tmpPath);
    std::string winDst = toWindowsPath(path);
    std::wstring wTmp(winTmp.begin(), winTmp.end());
    std::wstring wDst(winDst.begin(), winDst.end());
    if (!::MoveFileExW(wTmp.c_str(), wDst.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Fallback: direct overwrite
        std::error_code ec;
        fs::copy_file(tmpPath, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
    }
#else
    std::error_code ec;
    fs::rename(tmpPath, path, ec);
    if (ec) {
        // Fallback: copy + delete
        fs::copy_file(tmpPath, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
    }
#endif
}

static int clampDb(int db) {
    if (db < -60) return -60;
    if (db > 0) return 0;
    return db;
}

static int clampMeterDb(int db) {
    if (db < -60) return -60;
    if (db > 12) return 12;
    return db;
}

static int be16ToSigned(const unsigned char* p) {
    uint16_t u = (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    return (u & 0x8000) ? static_cast<int>(u) - 65536 : static_cast<int>(u);
}

static int be24ToSigned(const unsigned char* p) {
    int32_t u = (static_cast<int32_t>(p[0]) << 16)
              | (static_cast<int32_t>(p[1]) << 8)
              |  static_cast<int32_t>(p[2]);
    if (u & 0x800000) u |= static_cast<int32_t>(0xFF000000); // sign-extend
    return static_cast<int>(u);
}

// bitDepth: 16 = L16 (4 bytes/stereo frame), 24 = L24 (6 bytes/stereo frame)
static bool parseRtpStereoLevels(const char* data, size_t len, int& leftDb, int& rightDb, int bitDepth = 24) {
    if (!data || len < 16) return false;
    if (len <= 12) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data) + 12;
    size_t payloadLen = len - 12;
    const size_t bytesPerFrame = static_cast<size_t>(bitDepth / 8) * 2; // 2 channels
    size_t frames = payloadLen / bytesPerFrame;
    if (frames < 1) return false;

    double lsum = 0.0;
    double rsum = 0.0;
    const double normFactor = (bitDepth == 24) ? 8388608.0 : 32768.0;
    for (size_t i = 0; i < frames; ++i) {
        const unsigned char* fp = p + i * bytesPerFrame;
        int l, r;
        if (bitDepth == 24) {
            l = be24ToSigned(fp);
            r = be24ToSigned(fp + 3);
        } else {
            l = be16ToSigned(fp);
            r = be16ToSigned(fp + 2);
        }
        double ln = static_cast<double>(l) / normFactor;
        double rn = static_cast<double>(r) / normFactor;
        lsum += ln * ln;
        rsum += rn * rn;
    }

    double lrms = std::sqrt(lsum / static_cast<double>(frames));
    double rrms = std::sqrt(rsum / static_cast<double>(frames));
    if (lrms <= 1e-9) leftDb = -60;
    else leftDb = clampDb(static_cast<int>(std::lround(20.0 * std::log10(lrms))));
    if (rrms <= 1e-9) rightDb = -60;
    else rightDb = clampDb(static_cast<int>(std::lround(20.0 * std::log10(rrms))));
    return true;
}

static std::string singleLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

static std::string xmlField(const std::string& xml, const std::vector<std::string>& tags) {
    std::string lowerXml = xml;
    std::transform(lowerXml.begin(), lowerXml.end(), lowerXml.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& tag : tags) {
        std::string t = tag;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string open = "<" + t + ">";
        std::string close = "</" + t + ">";
        size_t b = lowerXml.find(open);
        if (b == std::string::npos) continue;
        b += open.size();
        size_t e = lowerXml.find(close, b);
        if (e == std::string::npos || e <= b) continue;
        return xml.substr(b, e - b);
    }
    return "";
}

// Parse a JSON array-of-strings like ["title","artist"] into a vector.
static std::vector<std::string> jsonArrayStrings(const std::string& raw) {
    std::vector<std::string> result;
    size_t i = 0;
    while (i < raw.size() && raw[i] != '[') ++i;
    if (i >= raw.size()) return result;
    ++i;
    while (i < raw.size()) {
        while (i < raw.size() && (std::isspace(static_cast<unsigned char>(raw[i])) || raw[i] == ',')) ++i;
        if (i >= raw.size() || raw[i] == ']') break;
        if (raw[i] == '"') {
            ++i;
            size_t start = i;
            while (i < raw.size() && raw[i] != '"') {
                if (raw[i] == '\\') ++i;
                ++i;
            }
            result.push_back(raw.substr(start, i - start));
            if (i < raw.size()) ++i;
        } else {
            ++i;
        }
    }
    return result;
}

static std::string upperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

static std::string jsonEscapeCompact(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n':
            case '\r':
            case '\t': out += ' '; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

static bool isSafeMetaForExtinf(const std::string& s, size_t maxLen = 1024) {
    if (s.empty()) return false;
    if (s.size() > maxLen) return false;
    for (unsigned char c : s) {
        if (c < 0x20 && c != '\t') return false;
        if (c == '\r' || c == '\n') return false;
    }
    return true;
}

static std::string sanitizeForExtinf(const std::string& in, size_t maxLen = 1024) {
    std::string out;
    out.reserve((std::min)(maxLen, in.size()));
    for (char c : in) {
        if (c == '\r' || c == '\n' || c == '\t') {
            out.push_back(' ');
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            out.push_back(c);
        }
        if (out.size() >= maxLen) break;
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

static std::string replaceExtinfPayload(const std::string& line, const std::string& payload) {
    size_t comma = line.find(',');
    if (comma == std::string::npos) return line + "," + payload;
    return line.substr(0, comma + 1) + payload;
}

// Inject metadata payload into an existing HLS index.m3u8 playlist.
// current: patch only the latest EXTINF line.
// currentFuture: patch all EXTINF lines currently in the playlist window.
static bool injectHlsMetadataIntoPlaylist(const std::string& cfgDir, const simplejson::Object& hlsCfg,
                                          const std::string& rawPayload, std::string& err) {
    if (!hlsCfg.getBool("metaEnabled", true)) {
        err = "hls meta disabled";
        return false;
    }

    simplejson::Object parser = hlsCfg.getSubObject("metaParser");
    const std::string scope = parser.getString("scope", "current");
    const bool includeFuture = (scope == "currentFuture");

    std::string payload = rawPayload;
    if (!isSafeMetaForExtinf(payload)) payload = sanitizeForExtinf(payload);
    if (!isSafeMetaForExtinf(payload)) {
        err = "payload not safe for EXTINF";
        return false;
    }

    const std::string playlistPath = cfgDir + "/hls/index.m3u8";
    std::ifstream in(playlistPath);
    if (!in.is_open()) {
        err = "playlist not found";
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(in)), {});
    in.close();
    if (content.empty()) {
        err = "playlist empty";
        return false;
    }

    std::vector<std::string> lines;
    std::vector<size_t> extinfIdx;
    {
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("#EXTINF:", 0) == 0) extinfIdx.push_back(lines.size());
            lines.push_back(line);
        }
    }

    if (extinfIdx.empty()) {
        err = "no EXTINF lines";
        return false;
    }

    if (includeFuture) {
        for (size_t idx : extinfIdx) {
            lines[idx] = replaceExtinfPayload(lines[idx], payload);
        }
    } else {
        size_t idx = extinfIdx.back();
        lines[idx] = replaceExtinfPayload(lines[idx], payload);
    }

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i] << "\n";
    }

    const std::string tmpPath = playlistPath + ".tmp";
    {
        std::ofstream tf(tmpPath, std::ios::trunc);
        if (!tf.is_open()) {
            err = "cannot open temp playlist";
            return false;
        }
        tf << out.str();
    }

    std::error_code ec;
    fs::rename(tmpPath, playlistPath, ec);
    if (ec) {
        fs::copy_file(tmpPath, playlistPath, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmpPath, ec);
        if (ec) {
            err = "failed replacing playlist";
            return false;
        }
    }

    return true;
}

static std::vector<std::string> extractNowPlayingBlocks(const std::string& xml) {
    std::vector<std::string> blocks;
    const std::string open = "<nowplaying>";
    const std::string close = "</nowplaying>";
    size_t pos = 0;
    while (true) {
        size_t b = xml.find(open, pos);
        if (b == std::string::npos) break;
        size_t e = xml.find(close, b);
        if (e == std::string::npos) break;
        e += close.size();
        blocks.push_back(xml.substr(b, e - b));
        pos = e;
    }
    return blocks;
}

static size_t selectCurrentNowPlayingIndex(const std::vector<std::string>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (xmlField(blocks[i], {"stack_pos"}) == "0") return i;
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (!xmlField(blocks[i], {"air_time"}).empty()) return i;
    }
    return 0;
}

static std::string fieldValueWithAliases(const std::string& xmlBlock, const std::string& tag,
                                         const std::string& stationId = "") {
    if (tag == "stationId") return stationId;
    if (tag == "sched_time") return xmlField(xmlBlock, {"sched_time"});
    if (tag == "stack_pos" || tag == "stack_position") return xmlField(xmlBlock, {"stack_pos"});
    if (tag == "title") return xmlField(xmlBlock, {"title", "song", "name"});
    if (tag == "artist") return xmlField(xmlBlock, {"artist", "performer"});
    if (tag == "duration") return xmlField(xmlBlock, {"duration", "length"});
    if (tag == "category") return xmlField(xmlBlock, {"category", "type"});
    if (tag == "trivia") return xmlField(xmlBlock, {"trivia"});
    if (tag == "cart") return xmlField(xmlBlock, {"cart"});
    if (tag == "media_type") return xmlField(xmlBlock, {"media_type"});
    if (tag == "station") return xmlField(xmlBlock, {"station"});
    if (tag == "album") return xmlField(xmlBlock, {"album", "trivia"});
    return xmlField(xmlBlock, {tag});
}

// Apply {field} tokens in a template string.
// Uses fieldValueWithAliases to expand any XML field (title, artist, duration,
// category, trivia, cart, media_type, station, stationId, etc.).
// Any {token} with no matching value is left as-is.
static std::string applyMetaTemplate(const std::string& tmpl, const std::string& artist,
                                     const std::string& title, const std::string& duration,
                                     const std::string& stationId,
                                     const std::string& xmlBlock = "") {
    std::string out = tmpl;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t open = out.find('{', pos);
        if (open == std::string::npos) break;
        size_t close = out.find('}', open + 1);
        if (close == std::string::npos) break;
        std::string key = out.substr(open + 1, close - open - 1);
        if (key.empty()) { pos = close + 1; continue; }

        std::string val;
        if (!xmlBlock.empty()) {
            val = fieldValueWithAliases(xmlBlock, key, stationId);
        }
        // Fallback for common fields when xmlBlock is not provided.
        if (val.empty()) {
            if      (key == "artist")    val = artist;
            else if (key == "title")     val = title;
            else if (key == "duration")  val = duration;
            else if (key == "stationId") val = stationId;
        }

        if (!val.empty()) {
            out.replace(open, close - open + 1, val);
            pos = open + val.size();
        } else {
            pos = close + 1;  // unknown/empty — leave literal
        }
    }
    return out;
}

static std::string id3FrameForTag(const std::string& tag, const std::string& value) {
    if (value.empty()) return "";
    if (tag == "title") return "TIT2=\"" + value + "\"";
    if (tag == "artist") return "TPE1=\"" + value + "\"";
    if (tag == "album" || tag == "trivia") return "TALB=\"" + value + "\"";
    if (tag == "category") return "TCON=\"" + value + "\"";
    if (tag == "duration") return "TLEN=\"" + value + "\"";
    return "TXXX-" + upperAscii(tag) + "=\"" + value + "\"";
}

static std::string hlsMetadataByParser(const std::string& xml, const simplejson::Object& streamCfg,
                                       const std::string& title, const std::string& artist,
                                       const std::string& duration) {
    simplejson::Object parser = streamCfg.getSubObject("metaParser");
    const std::string method = parser.getString("method", "id3");
    const std::string scope = parser.getString("scope", "current");
    const bool includeFuture = (scope == "currentFuture");
    const std::string stationId = streamCfg.getString("stationId", streamCfg.getString("streamId", ""));

    std::string tagsRaw = parser.getRawValue("tags", "");
    std::vector<std::string> tags;
    if (!tagsRaw.empty()) tags = jsonArrayStrings(tagsRaw);
    if (tags.empty()) tags = {"title", "artist", "category", "duration"};

    // Deduplicate while preserving order.
    std::vector<std::string> uniqueTags;
    std::set<std::string> seen;
    for (const auto& t : tags) {
        if (t.empty() || seen.count(t)) continue;
        seen.insert(t);
        uniqueTags.push_back(t);
    }
    tags = uniqueTags;

    auto blocks = extractNowPlayingBlocks(xml);
    std::string currentBlock = xml;
    std::vector<std::string> upcomingBlocks;
    if (!blocks.empty()) {
        size_t curIdx = selectCurrentNowPlayingIndex(blocks);
        if (curIdx < blocks.size()) currentBlock = blocks[curIdx];
        if (includeFuture) {
            for (const auto& b : blocks) {
                std::string sp = xmlField(b, {"stack_pos"});
                if (sp.empty()) continue;
                try {
                    if (std::stoi(sp) >= 1) upcomingBlocks.push_back(b);
                } catch (...) {
                }
            }
        }
    }

    if (method == "xmlPassthrough") {
        std::string payload = includeFuture ? xml : currentBlock;
        std::string oneLine = singleLine(payload);
        if (oneLine.size() > 600) oneLine = oneLine.substr(0, 600) + "...";
        return oneLine;
    }

    if (method == "ext") {
        // Produce Orban 5950HD-compatible EXT JSON matching the original encoder format.
        // Field mapping: media_type→"type", trivia→"album", cart→"id"
        // duration converted from milliseconds string → float seconds.
        // "start" estimated as current epoch (chained by duration for upcoming items).
        // "image" always present as "" for Orban compatibility.
        auto durSecsFromBlock = [](const std::string& block) -> double {
            std::string d = xmlField(block, {"duration", "length"});
            if (d.empty()) return 0.0;
            try { double v = std::stod(d); return v > 1000.0 ? v / 1000.0 : v; }
            catch (...) { return 0.0; }
        };
        auto makeExtItem = [&](const std::string& block, double startSecs) -> std::string {
            double durSecs = durSecsFromBlock(block);
            std::string type  = xmlField(block, {"media_type"});
            std::string ttl   = xmlField(block, {"title", "song", "name"});
            std::string album = xmlField(block, {"trivia"});
            std::string art   = xmlField(block, {"artist", "performer"});
            std::string cart  = xmlField(block, {"cart"});
            std::string cat   = xmlField(block, {"category"});
            std::ostringstream o;
            o << std::fixed << std::setprecision(3);
            o << "{\"type\":\""    << jsonEscapeCompact(type)  << "\"";
            o << ",\"title\":\""   << jsonEscapeCompact(ttl)   << "\"";
            o << ",\"album\":\""   << jsonEscapeCompact(album) << "\"";
            o << ",\"artist\":\""  << jsonEscapeCompact(art)   << "\"";
            o << ",\"image\":\"\"";
            o << ",\"duration\":"  << durSecs;
            o << ",\"start\":"    << startSecs;
            o << ",\"id\":\""      << jsonEscapeCompact(cart)  << "\"";
            if (!cat.empty())
                o << ",\"category\":\"" << jsonEscapeCompact(cat) << "\"";
            o << "}";
            return o.str();
        };
        // Current Unix epoch in seconds (millisecond precision).
        double nowSecs = std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ostringstream out;
        out << "{\"current\":" << makeExtItem(currentBlock, nowSecs);
        if (includeFuture && !upcomingBlocks.empty()) {
            out << ",\"upcoming\":[";
            double nextStart = nowSecs + durSecsFromBlock(currentBlock);
            for (size_t i = 0; i < upcomingBlocks.size(); ++i) {
                if (i) out << ",";
                out << makeExtItem(upcomingBlocks[i], nextStart);
                nextStart += durSecsFromBlock(upcomingBlocks[i]);
            }
            out << "]";
        }
        out << "}";
        return out.str();
    }

    // id3 (default)
    std::vector<std::string> frames;
    for (const auto& tag : tags) {
        std::string val = fieldValueWithAliases(currentBlock, tag, stationId);
        if (val.empty()) {
            if (tag == "title") val = title;
            else if (tag == "artist") val = artist;
            else if (tag == "duration") val = duration;
        }
        std::string frame = id3FrameForTag(tag, val);
        if (!frame.empty()) frames.push_back(frame);
    }
    std::string out = "ID3:";
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i) out += ";";
        out += frames[i];
    }
    if (includeFuture && !upcomingBlocks.empty()) {
        std::string upcoming;
        for (size_t i = 0; i < upcomingBlocks.size(); ++i) {
            std::string ut = fieldValueWithAliases(upcomingBlocks[i], "title");
            std::string ua = fieldValueWithAliases(upcomingBlocks[i], "artist");
            if (ut.empty() && ua.empty()) continue;
            if (!upcoming.empty()) upcoming += " | ";
            upcoming += (ua.empty() ? ut : (ua + " - " + ut));
        }
        if (!upcoming.empty()) {
            if (!out.empty() && out != "ID3:") out += ";";
            out += "TXXX-UPCOMING=\"" + upcoming + "\"";
        }
    }
    return out;
}

// Send an out-of-band ICY metadata title update to an Icecast server's admin API.
// This is a fire-and-forget function — the caller should run it in a detached thread.
// URL format: http://host[:port]/mountpoint  (or icecast:// — scheme is stripped).
static void sendIcecastMetaUpdate(const std::string& streamUrl,
                                  const std::string& user,
                                  const std::string& pass,
                                  const std::string& title) {
    // Strip scheme
    std::string rest = streamUrl;
    for (const auto& scheme : {"icecast://", "http://", "https://"}) {
        if (rest.rfind(scheme, 0) == 0) { rest = rest.substr(strlen(scheme)); break; }
    }
    // Strip embedded user:pass@ if present
    auto at = rest.find('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);

    auto slash = rest.find('/');
    if (slash == std::string::npos) return;
    std::string hostPort = rest.substr(0, slash);
    std::string mount    = rest.substr(slash); // keeps leading /

    std::string host;
    int port = 80;
    auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        try { port = std::stoi(hostPort.substr(colon + 1)); } catch (...) {}
    } else {
        host = hostPort;
    }
    if (host.empty() || mount.empty()) return;

    // URL-percent-encode the title
    std::string encoded;
    for (unsigned char c : title) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }

    // Base64-encode  user:pass  for HTTP Basic auth
    std::string creds = user + ":" + pass;
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64str;
    int val = 0, valb = -6;
    for (unsigned char c : creds) {
        val = (val << 8) + c; valb += 8;
        while (valb >= 0) { b64str.push_back(b64[(val >> valb) & 0x3F]); valb -= 6; }
    }
    if (valb > -6) b64str.push_back(b64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (b64str.size() % 4) b64str.push_back('=');

    std::string req =
        "GET /admin/metadata?mount=" + mount +
        "&mode=updinfo&song=" + encoded +
        "&charset=UTF-8 HTTP/1.0\r\n" +
        "Host: " + host + "\r\n" +
        "Authorization: Basic " + b64str + "\r\n" +
        "Connection: close\r\n\r\n";

    struct addrinfo hints{}, *ai = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &ai) != 0) return;
    int sock = static_cast<int>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (sock >= 0) {
#ifdef _WIN32
        DWORD tv = 3000;
        setsockopt((SOCKET)sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt((SOCKET)sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        struct timeval tv{3, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            ::send(sock, req.c_str(), static_cast<int>(req.size()), 0);
        }
        close_socket(sock);
    }
    freeaddrinfo(ai);
}

// Format a metadata XML payload for a specific stream, applying the stream's
// metaParser config saved in its config file.
static std::string formattedMetadataForStream(const std::string& streamTag, const std::string& xml,
                                               const simplejson::Object& streamCfg) {
    std::string title    = xmlField(xml, {"title", "song", "name"});
    std::string artist   = xmlField(xml, {"artist", "performer"});
    std::string duration = xmlField(xml, {"duration", "length"});
    std::string stationId = streamCfg.getString("stationId", "");

    if (title.empty() && artist.empty()) {
        std::string compact = singleLine(xml);
        if (compact.size() > 280) compact = compact.substr(0, 280) + "...";
        return compact;
    }

    // HLS: apply parser method + scope + selected tags.
    if (streamTag == "HLS") {
        return hlsMetadataByParser(xml, streamCfg, title, artist, duration);
    }

    // SRT keeps its fixed key=value style.
    if (streamTag == "SRT") {
        return "title=" + title + ";artist=" + artist + ";duration=" + duration;
    }

    // Resolve the current nowplaying block (stack_pos=0) so that {field}
    // tokens in a template are read from the right item only.
    auto blocks = extractNowPlayingBlocks(xml);
    const std::string& currentBlock = blocks.empty() ? xml : blocks[selectCurrentNowPlayingIndex(blocks)];

    // AAC / MP3: apply metaParser settings
    simplejson::Object parser = streamCfg.getSubObject("metaParser");
    std::string tpl = parser.getString("template", "");
    if (!tpl.empty()) {
        return applyMetaTemplate(tpl, artist, title, duration, stationId, currentBlock);
    }

    // Legacy mode: separator + fields list
    std::string sep = parser.getString("separator", " - ");
    bool incSid     = parser.getBool("includeStationId", false);
    std::string fieldsRaw = parser.getRawValue("fields", "");
    std::vector<std::string> fields;
    if (!fieldsRaw.empty()) fields = jsonArrayStrings(fieldsRaw);
    if (fields.empty()) fields = {"artist", "title"};

    std::vector<std::string> parts;
    for (const auto& f : fields) {
        std::string val;
        if (f == "title")    val = title;
        else if (f == "artist")   val = artist;
        else if (f == "duration") val = duration;
        else                      val = xmlField(xml, {f});
        if (!val.empty()) parts.push_back(val);
    }
    if (incSid && !stationId.empty()) parts.push_back(stationId);

    if (parts.empty()) return artist.empty() ? title : (artist + sep + title);
    std::string result;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) result += sep;
        result += parts[k];
    }
    return result;
}

static bool parsePcm16StereoLevelsLE(const char* data, size_t len, int& leftDb, int& rightDb) {
    if (!data || len < 4) return false;
    size_t frames = len / 4;
    if (frames < 1) return false;

    double lsum = 0.0;
    double rsum = 0.0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < frames; ++i) {
        int16_t l = static_cast<int16_t>((static_cast<uint16_t>(p[i * 4 + 1]) << 8) | p[i * 4]);
        int16_t r = static_cast<int16_t>((static_cast<uint16_t>(p[i * 4 + 3]) << 8) | p[i * 4 + 2]);
        double ln = static_cast<double>(l) / 32768.0;
        double rn = static_cast<double>(r) / 32768.0;
        lsum += ln * ln;
        rsum += rn * rn;
    }

    double lrms = std::sqrt(lsum / static_cast<double>(frames));
    double rrms = std::sqrt(rsum / static_cast<double>(frames));
    if (lrms <= 1e-9) leftDb = -60;
    else leftDb = clampDb(static_cast<int>(std::lround(20.0 * std::log10(lrms))));
    if (rrms <= 1e-9) rightDb = -60;
    else rightDb = clampDb(static_cast<int>(std::lround(20.0 * std::log10(rrms))));
    return true;
}

#ifdef _WIN32
template <typename T>
static void safeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static std::string wideToUtf8(const wchar_t* w);

static std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = toLower(haystack);
    std::string n = toLower(needle);
    return h.find(n) != std::string::npos;
}

struct WaveInputMeter {
    HWAVEIN hwi = nullptr;
    WAVEHDR hdrA{};
    WAVEHDR hdrB{};
    std::vector<char> bufA;
    std::vector<char> bufB;
    UINT devId = WAVE_MAPPER;
    int sampleRate = 48000;
    bool open = false;
};

struct WasapiInputMeter {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* mixFmt = nullptr;
    std::string endpointName;
    bool open = false;
};

static bool parseWaveDeviceId(const std::string& token, UINT& outId) {
    if (token.empty()) {
        outId = WAVE_MAPPER;
        return true;
    }
    try {
        int v = std::stoi(token);
        if (v < 0) return false;
        outId = static_cast<UINT>(v);
        return true;
    } catch (...) {
        return false;
    }
}

// Find a waveIn device by (partial, case-insensitive) name match.
// Used when the stored device token is a name string rather than a numeric index.
static bool findWaveDeviceByName(const std::string& name, UINT& outId) {
    if (name.empty()) return false;
    std::string nameLower = name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    UINT count = waveInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        WAVEINCAPSA caps{};
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::string capName = caps.szPname;
        std::string capLower = capName;
        std::transform(capLower.begin(), capLower.end(), capLower.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (capLower.find(nameLower) != std::string::npos ||
            nameLower.find(capLower) != std::string::npos) {
            outId = i;
            return true;
        }
    }
    return false;
}

static std::string getWaveDeviceName(UINT devId) {
    WAVEINCAPSA caps{};
    if (waveInGetDevCapsA(devId, &caps, sizeof(caps)) != MMSYSERR_NOERROR) return "";
    return std::string(caps.szPname);
}

static std::string resolveWindowsAudioDeviceForFfmpeg(const std::string& deviceToken) {
    if (deviceToken.empty()) return "";

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(coHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* coll = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr) && enumerator &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)) && coll) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(coll->Item(i, &device)) || !device) continue;

            IPropertyStore* store = nullptr;
            PROPVARIANT pv;
            PropVariantInit(&pv);
            std::string friendly;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                    pv.vt == VT_LPWSTR && pv.pwszVal) {
                    friendly = wideToUtf8(pv.pwszVal);
                }
            }
            PropVariantClear(&pv);
            safeRelease(store);
            safeRelease(device);

            if (!friendly.empty() &&
                (containsNoCase(friendly, deviceToken) || containsNoCase(deviceToken, friendly))) {
                safeRelease(coll);
                safeRelease(enumerator);
                if (shouldUninit) CoUninitialize();
                return friendly;
            }
        }
    }

    safeRelease(coll);
    safeRelease(enumerator);
    if (shouldUninit) CoUninitialize();

    UINT devId = WAVE_MAPPER;
    if (parseWaveDeviceId(deviceToken, devId) && devId != WAVE_MAPPER) {
        std::string exact = getWaveDeviceName(devId);
        if (!exact.empty()) return exact;
    }

    if (findWaveDeviceByName(deviceToken, devId)) {
        std::string exact = getWaveDeviceName(devId);
        if (!exact.empty()) return exact;
    }

    return deviceToken;
}

static void closeWaveInput(WaveInputMeter& m) {
    if (!m.open || !m.hwi) return;
    waveInStop(m.hwi);
    waveInReset(m.hwi);
    waveInUnprepareHeader(m.hwi, &m.hdrA, sizeof(m.hdrA));
    waveInUnprepareHeader(m.hwi, &m.hdrB, sizeof(m.hdrB));
    waveInClose(m.hwi);
    m.hwi = nullptr;
    m.open = false;
}

static void closeWasapiInput(WasapiInputMeter& m) {
    if (m.client) m.client->Stop();
    safeRelease(m.capture);
    safeRelease(m.client);
    safeRelease(m.device);
    if (m.mixFmt) {
        CoTaskMemFree(m.mixFmt);
        m.mixFmt = nullptr;
    }
    m.endpointName.clear();
    m.open = false;
}

static bool openWaveInput(WaveInputMeter& m, UINT devId, int sampleRate, std::string& errMsg) {
    closeWaveInput(m);

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = static_cast<DWORD>(sampleRate > 0 ? sampleRate : 48000);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

    MMRESULT r = waveInOpen(&m.hwi, devId, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR || !m.hwi) {
        errMsg = "waveInOpen failed for device id " + std::to_string(devId);
        return false;
    }

    m.bufA.assign(static_cast<size_t>(fmt.nAvgBytesPerSec / 10), 0);
    m.bufB.assign(static_cast<size_t>(fmt.nAvgBytesPerSec / 10), 0);

    m.hdrA = WAVEHDR{};
    m.hdrA.lpData = m.bufA.data();
    m.hdrA.dwBufferLength = static_cast<DWORD>(m.bufA.size());
    m.hdrB = WAVEHDR{};
    m.hdrB.lpData = m.bufB.data();
    m.hdrB.dwBufferLength = static_cast<DWORD>(m.bufB.size());

    if (waveInPrepareHeader(m.hwi, &m.hdrA, sizeof(m.hdrA)) != MMSYSERR_NOERROR ||
        waveInPrepareHeader(m.hwi, &m.hdrB, sizeof(m.hdrB)) != MMSYSERR_NOERROR) {
        errMsg = "waveInPrepareHeader failed";
        closeWaveInput(m);
        return false;
    }

    if (waveInAddBuffer(m.hwi, &m.hdrA, sizeof(m.hdrA)) != MMSYSERR_NOERROR ||
        waveInAddBuffer(m.hwi, &m.hdrB, sizeof(m.hdrB)) != MMSYSERR_NOERROR) {
        errMsg = "waveInAddBuffer failed";
        closeWaveInput(m);
        return false;
    }

    if (waveInStart(m.hwi) != MMSYSERR_NOERROR) {
        errMsg = "waveInStart failed";
        closeWaveInput(m);
        return false;
    }

    m.devId = devId;
    m.sampleRate = sampleRate;
    m.open = true;
    errMsg.clear();
    return true;
}

static bool pollWaveLevels(WaveInputMeter& m, int& leftDb, int& rightDb, bool& hadData) {
    hadData = false;
    leftDb = -60;
    rightDb = -60;
    if (!m.open || !m.hwi) return false;

    auto processHeader = [&](WAVEHDR& h) {
        if ((h.dwFlags & WHDR_DONE) && h.dwBytesRecorded >= 4) {
            int l = -60;
            int r = -60;
            if (parsePcm16StereoLevelsLE(h.lpData, static_cast<size_t>(h.dwBytesRecorded), l, r)) {
                leftDb = l;
                rightDb = r;
                hadData = true;
            }
            h.dwBytesRecorded = 0;
            h.dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(m.hwi, &h, sizeof(h));
        }
    };

    processHeader(m.hdrA);
    processHeader(m.hdrB);
    return true;
}

static bool openWasapiInput(WasapiInputMeter& m, const std::string& deviceToken, std::string& errMsg) {
    closeWasapiInput(m);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* coll = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        errMsg = "WASAPI: cannot create MMDeviceEnumerator";
        safeRelease(enumerator);
        return false;
    }

    std::string preferredName;
    UINT devId = WAVE_MAPPER;
    if (parseWaveDeviceId(deviceToken, devId) && devId != WAVE_MAPPER) {
        preferredName = getWaveDeviceName(devId);
    }
    if (preferredName.empty() && !deviceToken.empty()) {
        // Token is a name string (not a numeric waveIn index) — use it directly for WASAPI matching
        preferredName = deviceToken;
    }

    IMMDevice* selected = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll);
    if (SUCCEEDED(hr) && coll && !preferredName.empty()) {
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d)) || !d) continue;

            IPropertyStore* store = nullptr;
            PROPVARIANT pv;
            PropVariantInit(&pv);
            std::string friendly;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &store)) && store) {
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    friendly = wideToUtf8(pv.pwszVal);
                }
            }
            PropVariantClear(&pv);
            safeRelease(store);

            if (containsNoCase(friendly, preferredName) || containsNoCase(preferredName, friendly)) {
                selected = d;
                break;
            }
            safeRelease(d);
        }
    }

    if (!selected) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &selected);
        if (FAILED(hr) || !selected) {
            errMsg = "WASAPI: default capture endpoint not available";
            safeRelease(coll);
            safeRelease(enumerator);
            return false;
        }
    }

    IPropertyStore* store = nullptr;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string endpointName;
    if (SUCCEEDED(selected->OpenPropertyStore(STGM_READ, &store)) && store) {
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            endpointName = wideToUtf8(pv.pwszVal);
        }
    }
    PropVariantClear(&pv);
    safeRelease(store);

    IAudioClient* client = nullptr;
    hr = selected->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr) || !client) {
        errMsg = "WASAPI: activate IAudioClient failed";
        safeRelease(selected);
        safeRelease(coll);
        safeRelease(enumerator);
        return false;
    }

    WAVEFORMATEX* mixFmt = nullptr;
    hr = client->GetMixFormat(&mixFmt);
    if (FAILED(hr) || !mixFmt) {
        errMsg = "WASAPI: GetMixFormat failed";
        safeRelease(client);
        safeRelease(selected);
        safeRelease(coll);
        safeRelease(enumerator);
        return false;
    }

    REFERENCE_TIME bufferDuration = 10000000;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                            bufferDuration, 0, mixFmt, nullptr);
    if (FAILED(hr)) {
        errMsg = "WASAPI: Initialize(shared) failed";
        CoTaskMemFree(mixFmt);
        safeRelease(client);
        safeRelease(selected);
        safeRelease(coll);
        safeRelease(enumerator);
        return false;
    }

    IAudioCaptureClient* cap = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap));
    if (FAILED(hr) || !cap) {
        errMsg = "WASAPI: GetService(IAudioCaptureClient) failed";
        CoTaskMemFree(mixFmt);
        safeRelease(client);
        safeRelease(selected);
        safeRelease(coll);
        safeRelease(enumerator);
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        errMsg = "WASAPI: Start failed";
        safeRelease(cap);
        CoTaskMemFree(mixFmt);
        safeRelease(client);
        safeRelease(selected);
        safeRelease(coll);
        safeRelease(enumerator);
        return false;
    }

    m.device = selected;
    m.client = client;
    m.capture = cap;
    m.mixFmt = mixFmt;
    m.endpointName = endpointName;
    m.open = true;

    safeRelease(coll);
    safeRelease(enumerator);
    errMsg.clear();
    return true;
}

static bool pollWasapiLevels(WasapiInputMeter& m, int& leftDb, int& rightDb, bool& hadData) {
    leftDb = -60;
    rightDb = -60;
    hadData = false;
    if (!m.open || !m.capture || !m.mixFmt) return false;

    UINT32 packetFrames = 0;
    HRESULT hr = m.capture->GetNextPacketSize(&packetFrames);
    if (FAILED(hr)) return false;

    double lsum = 0.0;
    double rsum = 0.0;
    uint64_t totalFrames = 0;

    while (packetFrames > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = m.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) return false;

        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        int channels = static_cast<int>(m.mixFmt->nChannels > 0 ? m.mixFmt->nChannels : 2);

        if (!silent && data && frames > 0) {
            if (m.mixFmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && m.mixFmt->wBitsPerSample == 32) {
                const float* f = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    float lv = f[i * channels];
                    float rv = (channels > 1) ? f[i * channels + 1] : lv;
                    lsum += static_cast<double>(lv) * static_cast<double>(lv);
                    rsum += static_cast<double>(rv) * static_cast<double>(rv);
                }
                totalFrames += frames;
                hadData = true;
            } else if (m.mixFmt->wFormatTag == WAVE_FORMAT_PCM && m.mixFmt->wBitsPerSample == 16) {
                const int16_t* s = reinterpret_cast<const int16_t*>(data);
                for (UINT32 i = 0; i < frames; ++i) {
                    double lv = static_cast<double>(s[i * channels]) / 32768.0;
                    double rv = static_cast<double>((channels > 1) ? s[i * channels + 1] : s[i * channels]) / 32768.0;
                    lsum += lv * lv;
                    rsum += rv * rv;
                }
                totalFrames += frames;
                hadData = true;
            }
        }

        m.capture->ReleaseBuffer(frames);
        hr = m.capture->GetNextPacketSize(&packetFrames);
        if (FAILED(hr)) return false;
    }

    if (hadData && totalFrames > 0) {
        double lrms = std::sqrt(lsum / static_cast<double>(totalFrames));
        double rrms = std::sqrt(rsum / static_cast<double>(totalFrames));
        leftDb = (lrms <= 1e-9) ? -60 : clampDb(static_cast<int>(std::lround(20.0 * std::log10(lrms))));
        rightDb = (rrms <= 1e-9) ? -60 : clampDb(static_cast<int>(std::lround(20.0 * std::log10(rrms))));
    }
    return true;
}
#endif

static bool isMulticastAddress(const std::string& ip) {
    in_addr a{};
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    uint32_t host = ntohl(a.s_addr);
    uint8_t first = static_cast<uint8_t>((host >> 24) & 0xFF);
    return first >= 224 && first <= 239;
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

static bool resolveInterfaceIPv4(const std::string& ifaceToken, in_addr& out) {
    out.s_addr = htonl(INADDR_ANY);
    if (ifaceToken.empty()) return true;

    in_addr direct{};
    if (inet_pton(AF_INET, ifaceToken.c_str(), &direct) == 1) {
        out = direct;
        return true;
    }

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_INET;
    ULONG size = 15000;
    std::vector<unsigned char> buffer(size);
    PIP_ADAPTER_ADDRESSES addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    ULONG rc = GetAdaptersAddresses(family, flags, nullptr, addrs, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        rc = GetAdaptersAddresses(family, flags, nullptr, addrs, &size);
    }
    if (rc != NO_ERROR) return false;

    for (PIP_ADAPTER_ADDRESSES p = addrs; p; p = p->Next) {
        std::string adapterName = p->AdapterName ? p->AdapterName : "";
        std::string friendly = wideToUtf8(p->FriendlyName);
        if (ifaceToken != adapterName && ifaceToken != friendly) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            out = sin->sin_addr;
            return true;
        }
    }
    return false;
}
#else
static bool resolveInterfaceIPv4(const std::string& ifaceToken, in_addr& out) {
    out.s_addr = htonl(INADDR_ANY);
    if (ifaceToken.empty()) return true;

    in_addr direct{};
    if (inet_pton(AF_INET, ifaceToken.c_str(), &direct) == 1) {
        out = direct;
        return true;
    }

    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr) continue;
        if (ifaceToken != p->ifa_name) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        out = sin->sin_addr;
        found = true;
        break;
    }
    freeifaddrs(ifa);
    return found;
}
#endif

static int openRtpInputSocket(const std::string& rtpAddr, int rtpPort, const std::string& ifaceToken, std::string& errMsg) {
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        errMsg = "socket() failed";
        return -1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(rtpPort));
    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        errMsg = "bind() failed on port " + std::to_string(rtpPort);
        close_socket(sfd);
        return -1;
    }

    if (!rtpAddr.empty() && isMulticastAddress(rtpAddr)) {
        in_addr ifaceAddr{};
        if (!resolveInterfaceIPv4(ifaceToken, ifaceAddr)) {
            errMsg = "interface not found or has no IPv4 address: " + ifaceToken;
            close_socket(sfd);
            return -1;
        }

        ip_mreq mreq{};
        in_addr multi{};
        if (inet_pton(AF_INET, rtpAddr.c_str(), &multi) != 1) {
            errMsg = "invalid multicast address: " + rtpAddr;
            close_socket(sfd);
            return -1;
        }
        mreq.imr_multiaddr = multi;
        mreq.imr_interface = ifaceAddr;
#ifdef _WIN32
        if (setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) != 0) {
            errMsg = "IP_ADD_MEMBERSHIP failed for " + rtpAddr;
            close_socket(sfd);
            return -1;
        }
#else
        if (setsockopt(sfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            errMsg = "IP_ADD_MEMBERSHIP failed for " + rtpAddr;
            close_socket(sfd);
            return -1;
        }
#endif
    }

    errMsg.clear();
    return sfd;
}

// ---------- helpers ----------

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    char ms_buf[8];
    snprintf(ms_buf, sizeof(ms_buf), ".%03dZ", (int)ms.count());
    return std::string(buf) + ms_buf;
}

// ---------- Worker ----------

Worker::Worker(int idx, const std::string& cfgDir)
    : m_idx(idx), m_cfgDir(cfgDir) {
    m_logPath = cfgDir + "/logs/Encoder" + std::to_string(idx) + ".log";
    m_logFile.open(m_logPath, std::ios::app);
    log("Worker " + std::to_string(idx) + " initialised");
}

Worker::~Worker() {
    m_running = false;
    // Kill any running FFmpeg processes before joining threads
    killFfmpegProc(m_aacProc);
    killFfmpegProc(m_mp3Proc);
    killFfmpegProc(m_hlsProc);
    // Stop HLS HTTP server before joining threads
    if (m_hlsHttpRunning.load()) {
        m_hlsHttpRunning = false;
        int s = m_hlsHttpSocket.load();
        if (s != -1) close_socket(s);
    }
    if (m_hlsSegMonRunning.load()) m_hlsSegMonRunning = false;
    if (m_hlsHttpThread.joinable())   m_hlsHttpThread.join();
    if (m_hlsSegMonThread.joinable()) m_hlsSegMonThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();
    if (m_metaThread.joinable())    m_metaThread.join();
    if (m_inputLevelThread.joinable()) m_inputLevelThread.join();
}

void Worker::logSys(const std::string& msg) {
    fs::create_directories("/etc/MC");
    std::ofstream f("/etc/MC/EncoderSys.log", std::ios::app);
    if (!f.is_open()) return;
    f << "[" << timestamp() << "] [Encoder-" << m_idx << "] " << msg << "\n";
}

void Worker::log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_logMutex);
    std::string line = "[" + timestamp() + "] [Encoder-" + std::to_string(m_idx) + "] " + msg;
    std::cout << line << "\n";
    if (m_logFile.is_open()) {
        m_logFile << line << "\n";
        m_logFile.flush();
        // Rotation: check file size every write
        auto pos = m_logFile.tellp();
        if (pos > 10 * 1024 * 1024) {  // 10MB
            m_logFile.close();
            // Rename to .1 (simple rotation)
            std::string rotPath = m_logPath + ".1";
            fs::rename(m_logPath, rotPath);
            m_logFile.open(m_logPath, std::ios::app);
        }
    }
}

void Worker::run() {
    signal(SIGTERM, sigHandler);
    signal(SIGINT,  sigHandler);
    initSocketsOnce();
    m_running = true;

    // Start control and meta listener threads
    m_controlThread = std::thread([this]() { listenControlPort(); });
    m_metaThread    = std::thread([this]() { listenMetaPort();    });
    m_inputLevelThread = std::thread([this]() { monitorInputLevels(); });

    log("Worker running. Waiting for start commands.");
    publishStreamHealth();

    while (!g_workerShutdown && m_running) {
        // Heartbeat nominally every 30 s, but poll the shutdown flags every 500 ms
        // so SIGTERM/SIGINT is acted on quickly instead of waiting a full 30 s.
        for (int i = 0; i < 60 && !g_workerShutdown && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            pollSinkProcesses();
        }
        if (!m_running || g_workerShutdown) break;
        log("Heartbeat — AAC:" + std::string(m_aacRunning ? "LIVE" : "stopped")
            + " MP3:" + std::string(m_mp3Running ? "LIVE" : "stopped")
            + " HLS:" + std::string(m_hlsRunning ? "LIVE" : "stopped")
            + " SRT:" + std::string(m_srtRunning ? "LIVE" : "stopped"));
        publishStreamHealth();
    }
    m_running = false;
    log("Worker shutting down.");
}

void Worker::publishStreamHealth() {
    std::lock_guard<std::mutex> lk(m_rtMutex);
    simplejson::Object rt = readRuntimeState(m_cfgDir);
    rt.setInt("workerHeartbeatEpoch", static_cast<int>(std::time(nullptr)));
    rt.setBool("workerAacRunning", m_aacRunning.load());
    rt.setBool("workerMp3Running", m_mp3Running.load());
    rt.setBool("workerHlsRunning", m_hlsRunning.load());
    rt.setBool("workerSrtRunning", m_srtRunning.load());
    rt.setBool("controlListenerRunning", m_controlListenerRunning.load());
    writeRuntimeState(m_cfgDir, rt);
}

void Worker::pollSinkProcesses() {
    bool changed = false;

    if (m_aacRunning.load() && !ffmpegProcAlive(m_aacProc)) {
        m_aacProc = nullptr;
        m_aacRunning = false;
        log("AAC Icecast process exited; marking stream stopped");
        logSys("AAC Icecast process exited; marking stream stopped");
        changed = true;
    }

    if (m_mp3Running.load() && !ffmpegProcAlive(m_mp3Proc)) {
        m_mp3Proc = nullptr;
        m_mp3Running = false;
        log("MP3 Icecast process exited; marking stream stopped");
        logSys("MP3 Icecast process exited; marking stream stopped");
        changed = true;
    }

    if (m_hlsRunning.load() && !ffmpegProcAlive(m_hlsProc)) {
        m_hlsProc = nullptr;
        m_hlsRunning = false;
        log("HLS process exited; marking stream stopped");
        logSys("HLS process exited; marking stream stopped");
        changed = true;
    }

    if (m_srtRunning.load() && !ffmpegProcAlive(m_srtProc)) {
        m_srtProc = nullptr;
        m_srtRunning = false;
        log("SRT process exited; marking stream stopped");
        logSys("SRT process exited; marking stream stopped");
        changed = true;
    }

    if (changed) publishStreamHealth();
}

// ---------- FFmpeg helpers ----------

#ifdef _WIN32
// Resolve a Windows network adapter GUID string (e.g. "{26ABA82B-...}")
// to its first IPv4 unicast address (e.g. "192.168.87.38").
// FFmpeg's localaddr option requires an IP, not a GUID.
static std::string resolveAdapterGuidToIp(const std::string& guidStr) {
    ULONG bufLen = 15000;
    std::vector<BYTE> addrBuf(bufLen);
    auto* pAddr = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(addrBuf.data());
    if (GetAdaptersAddresses(AF_INET, 0, nullptr, pAddr, &bufLen) != ERROR_SUCCESS)
        return "";
    // AdapterName is the GUID in {GUID} form (ANSI). Compare case-insensitively.
    std::string norm = guidStr;
    for (char& c : norm) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    for (auto* p = pAddr; p; p = p->Next) {
        std::string aname(p->AdapterName);
        for (char& c : aname) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (aname != norm) continue;
        for (auto* ua = p->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ipStr[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sa->sin_addr, ipStr, sizeof(ipStr));
            return ipStr;
        }
    }
    return "";
}
#endif

std::vector<std::string> Worker::buildFfmpegInputArgs() {
    auto inp = readJsonFile(m_cfgDir + "/input.json");
    // Session params in runtime_state.json (written by supervisor's input/connect API)
    // take precedence over the persisted input.json values so that the FFmpeg input
    // matches exactly what the user connected to in the UI.
    auto rt  = readRuntimeState(m_cfgDir);
    std::string type = rt.getString("sessionInputType", inp.getString("inputType", "rtp"));
    std::vector<std::string> args;

    // Helper: given an interface spec (IP, Linux ifname, or Windows GUID),
    // return a usable IPv4 string for FFmpeg localaddr.
    auto resolveLocalIp = [](const std::string& iface) -> std::string {
        if (iface.empty()) return "";

        in_addr resolved{};
        if (resolveInterfaceIPv4(iface, resolved)) {
            char ipStr[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &resolved, ipStr, sizeof(ipStr))) {
                return std::string(ipStr);
            }
        }

        // Compatibility fallback for GUID token handling if interface lookup fails.
#ifdef _WIN32
        if (iface[0] == '{') {
            return resolveAdapterGuidToIp(iface);
        }
#endif
        return "";
    };

    // Helper: write an SDP file for L16/L24 RTP multicast and return FFmpeg input args.
    // Axia Livewire (and generic RTP) use dynamic payload type 96 which FFmpeg cannot
    // detect without an SDP descriptor. We write the SDP and tell FFmpeg to read it.
    // bitDepth: 16 = L16 (standard), 24 = L24 (Axia Livewire standard channels).
    auto buildRtpSdpArgs = [this, &resolveLocalIp](
            const std::string& addr, int port, const std::string& iface,
            int channels, int sampleRate, int bitDepth) -> std::vector<std::string>
    {
        std::string sdpPath = m_cfgDir + "/input_rtp.sdp";
#ifdef _WIN32
        {
            // Resolve to absolute Windows path so FFmpeg can stat() the file
            char absBuf[MAX_PATH] = {};
            if (_fullpath(absBuf, sdpPath.c_str(), MAX_PATH)) sdpPath = absBuf;
            for (char& c : sdpPath) if (c == '/') c = '\\';
        }
#endif
        {
            std::ofstream sdp(sdpPath);
            sdp << "v=0\r\n"
                << "o=- 0 0 IN IP4 0.0.0.0\r\n"
                << "s=RTP Input\r\n"
                << "c=IN IP4 " << addr << "\r\n"
                << "t=0 0\r\n"
                << "m=audio " << port << " RTP/AVP 96\r\n"
                << "a=rtpmap:96 L" << bitDepth << "/" << sampleRate << "/" << channels << "\r\n";
        }
        std::vector<std::string> a;
        a.push_back("-protocol_whitelist"); a.push_back("file,rtp,udp");
        std::string localIp = resolveLocalIp(iface);
        if (!iface.empty() && localIp.empty()) {
            log("FFmpeg RTP input: interface '" + iface + "' could not be resolved to IPv4; omitting -localaddr");
        } else if (!iface.empty() && localIp != iface) {
            log("FFmpeg RTP input: interface '" + iface + "' resolved to localaddr=" + localIp);
        }
        if (!localIp.empty()) {
            a.push_back("-localaddr"); a.push_back(localIp);
        }
        a.push_back("-i"); a.push_back(sdpPath);
        return a;
    };

    if (type == "rtp") {
        std::string addr  = rt.getString("sessionRtpAddress",   inp.getString("rtpAddress", "239.192.0.0"));
        int         port  = rt.getInt   ("sessionRtpPort",      inp.getInt("rtpPort", 5004));
        std::string iface = rt.getString("sessionRtpInterface", inp.getString("rtpInterface", ""));
        int channels   = inp.getInt("channels", 2);
        int sampleRate = rt.getInt("sessionSampleRate", inp.getInt("sampleRate", 48000));
        int bitDepth   = inp.getInt("bitDepth", 24); // L24 is default for modern Axia/AES67
        args = buildRtpSdpArgs(addr, port, iface, channels, sampleRate, bitDepth);
    } else if (type == "axia") {
        // Axia Livewire Standard Channels: L24 PCM RTP multicast, dynamic payload type 96.
        // Requires SDP file so FFmpeg knows the codec without RTCP negotiation.
        // bitDepth defaults to 24 — Axia Standard channels use 24-bit samples.
        std::string addr  = rt.getString("sessionRtpAddress",   inp.getString("rtpAddress", "239.192.0.0"));
        int         port  = rt.getInt   ("sessionRtpPort",      inp.getInt("rtpPort", 5004));
        std::string iface = rt.getString("sessionRtpInterface", inp.getString("rtpInterface", ""));
        int sampleRate = rt.getInt("sessionSampleRate", inp.getInt("sampleRate", 48000));
        int bitDepth   = inp.getInt("bitDepth", 24);
        // Axia Livewire standard: 2-channel stereo (primary pair)
        args = buildRtpSdpArgs(addr, port, iface, 2, sampleRate, bitDepth);
    } else if (type == "audio") {
        // Local audio device.
        // audioDevice is the device's friendly name (Windows waveIn name / Linux ALSA ID).
        std::string deviceName = rt.getString("sessionAudioDevice", inp.getString("audioDevice", ""));
#ifdef _WIN32
        // Windows DirectShow: "audio=<friendly name>" or fallback to first waveIn device.
        std::string resolvedDeviceName = resolveWindowsAudioDeviceForFfmpeg(deviceName);
        if (!deviceName.empty() && resolvedDeviceName != deviceName) {
            log("FFmpeg audio device resolved from '" + deviceName + "' to '" + resolvedDeviceName + "'");
        }
        std::string dshowDev = resolvedDeviceName.empty()
            ? "audio=@device_cm_{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\wave_{0}"
            : ("audio=" + resolvedDeviceName);
        args = {"-f", "dshow", "-i", dshowDev};
#else
        // Linux ALSA: device ID is e.g. "hw:0", "hw:1", or "default".
        std::string alsaDev = deviceName.empty() ? "default" : deviceName;
        args = {"-f", "alsa", "-ac", "2", "-ar", "48000", "-i", alsaDev};
#endif
    } else if (type == "srt") {
        std::string host = inp.getString("srtHost", "127.0.0.1");
        int         port = inp.getInt("srtPort", 9250);
        int         latency = inp.getInt("srtLatency", 120);
        std::string pass = inp.getString("srtPass", "");
        std::string uri = "srt://" + host + ":" + std::to_string(port) + "?mode=caller";
        if (latency > 0) uri += "&latency=" + std::to_string(latency);
        if (!pass.empty()) uri += "&passphrase=" + pass;
        args = {"-re", "-i", uri};
    } else {
        // Fallback: treat as generic RTP multicast
        std::string addr     = rt.getString("sessionRtpAddress",   inp.getString("rtpAddress", "239.192.0.0"));
        int         port     = rt.getInt   ("sessionRtpPort",      inp.getInt("rtpPort", 5004));
        std::string iface    = rt.getString("sessionRtpInterface", inp.getString("rtpInterface", ""));
        int         bitDepth = inp.getInt("bitDepth", 24);
        args = buildRtpSdpArgs(addr, port, iface, 2, 48000, bitDepth);
    }
    return args;
}

#ifdef _WIN32
// Locate ffmpeg.exe, searching in order:
//   1. Current process PATH (via SearchPathW)
//   2. Machine + User PATH read fresh from the registry (handles stale inherited env)
//   3. WinGet packages folder under USERPROFILE
//   4. Common manual install locations
// Result is cached after the first successful lookup.
static std::string findFfmpegExe() {
    static std::string cached;
    if (!cached.empty()) return cached;

    // 0. Same directory as this executable (bundled ffmpeg.exe)
    {
        wchar_t selfBuf[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, selfBuf, MAX_PATH)) {
            std::wstring selfPath(selfBuf);
            auto slash = selfPath.rfind(L'\\');
            if (slash != std::wstring::npos) {
                std::wstring candidate = selfPath.substr(0, slash + 1) + L"ffmpeg.exe";
                if (fs::exists(candidate)) {
                    cached = std::string(candidate.begin(), candidate.end());
                    return cached;
                }
            }
        }
    }

    // 1. Current process PATH
    wchar_t foundBuf[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"ffmpeg", L".exe", MAX_PATH, foundBuf, nullptr)) {
        std::wstring wf(foundBuf);
        cached = std::string(wf.begin(), wf.end());
        return cached;
    }

    // 2. Registry PATH (fresh, not inherited from parent process)
    auto readRegSz = [](HKEY root, const wchar_t* sub, const wchar_t* val) -> std::wstring {
        DWORD sz = 0;
        if (RegGetValueW(root, sub, val,
                RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_SZ | RRF_NOEXPAND,
                nullptr, nullptr, &sz) != ERROR_SUCCESS || sz == 0) return {};
        std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 1);
        if (RegGetValueW(root, sub, val,
                RRF_RT_REG_EXPAND_SZ | RRF_RT_REG_SZ | RRF_NOEXPAND,
                nullptr, buf.data(), &sz) != ERROR_SUCCESS) return {};
        return std::wstring(buf.data());
    };
    std::wstring sysPath  = readRegSz(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Path");
    std::wstring userPath = readRegSz(HKEY_CURRENT_USER, L"Environment", L"Path");
    std::wstring combined = sysPath + L";" + userPath;

    // Expand any %vars% in the combined path
    wchar_t expBuf[32768] = {};
    if (ExpandEnvironmentStringsW(combined.c_str(), expBuf, 32767)) combined = expBuf;

    std::wistringstream wss(combined);
    std::wstring wdir;
    while (std::getline(wss, wdir, L';')) {
        if (wdir.empty()) continue;
        while (!wdir.empty() && (wdir.back() == L' ' || wdir.back() == L'\r')) wdir.pop_back();
        std::wstring candidate = wdir + L"\\ffmpeg.exe";
        if (fs::exists(candidate)) {
            std::string s(candidate.begin(), candidate.end());
            cached = s;
            return cached;
        }
    }

    // 3. WinGet packages folder
    char upBuf[MAX_PATH] = {};
    GetEnvironmentVariableA("USERPROFILE", upBuf, sizeof(upBuf));
    if (upBuf[0]) {
        std::string winGet = std::string(upBuf)
            + "\\AppData\\Local\\Microsoft\\WinGet\\Packages";
        if (fs::exists(winGet)) {
            for (auto& pe : fs::directory_iterator(winGet, fs::directory_options::skip_permission_denied)) {
                std::string dn = pe.path().filename().string();
                if (dn.find("FFmpeg") != std::string::npos ||
                    dn.find("ffmpeg") != std::string::npos) {
                    for (auto& se : fs::recursive_directory_iterator(
                            pe.path(), fs::directory_options::skip_permission_denied)) {
                        if (se.path().filename().string() == "ffmpeg.exe") {
                            cached = se.path().string();
                            return cached;
                        }
                    }
                }
            }
        }
    }

    // 4. Common manual install locations
    for (const char* p : {
            "C:\\ffmpeg\\bin\\ffmpeg.exe",
            "C:\\Program Files\\ffmpeg\\bin\\ffmpeg.exe",
            "C:\\Program Files (x86)\\ffmpeg\\bin\\ffmpeg.exe"}) {
        if (fs::exists(p)) { cached = p; return cached; }
    }

    return {};
}
#endif  // _WIN32

void* Worker::launchFfmpeg(const std::vector<std::string>& args, const std::string& tag) {
#ifdef _WIN32
    if (args.empty()) return nullptr;

    // Resolve the ffmpeg executable to a full path so the launch succeeds even
    // when the process inherited a stale PATH (e.g. FFmpeg was installed after
    // the supervisor was started).
    std::string exePath = findFfmpegExe();
    if (exePath.empty()) {
        log("[" + tag + "] FAILED — ffmpeg.exe not found. "
            "Install FFmpeg and add it to PATH, or restart the supervisor after installation.");
        return nullptr;
    }
    log("[" + tag + "] Using FFmpeg: " + exePath);

    // Build command line: replace args[0] ("ffmpeg") with the resolved full path
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdLine += ' ';
        const std::string& a = (i == 0) ? exePath : args[i];
        bool needsQuote = a.find(' ') != std::string::npos;
        if (needsQuote) cmdLine += '"';
        cmdLine += a;
        if (needsQuote) cmdLine += '"';
    }
    log("[" + tag + "] FFmpeg command: " + cmdLine);
    std::wstring wExe(exePath.begin(), exePath.end());
    std::wstring wCmd(cmdLine.begin(), cmdLine.end());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring wLogPath(m_logPath.begin(), m_logPath.end());
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;
    HANDLE hLog = ::CreateFileW(
        wLogPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hLog != INVALID_HANDLE_VALUE) {
        ::SetFilePointer(hLog, 0, nullptr, FILE_END);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hLog;
        si.hStdError = hLog;
    }

    BOOL ok = CreateProcessW(
        wExe.c_str(), &wCmd[0],
        nullptr, nullptr,
        hLog != INVALID_HANDLE_VALUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);

    if (!ok) {
        log("[" + tag + "] FAILED to start FFmpeg — error " + std::to_string(GetLastError()));
        return nullptr;
    }
    CloseHandle(pi.hThread);
    log("[" + tag + "] FFmpeg process started (PID " + std::to_string(pi.dwProcessId) + ")");
    return pi.hProcess;
#else
    if (args.empty()) return nullptr;

    // Build argv for execvp: ["ffmpeg", arg1, arg2, ..., nullptr]
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        // Replace placeholder "ffmpeg" token with actual executable name
        argv.push_back(args[i].c_str());
    }
    argv.push_back(nullptr);

    // Log the command
    std::string cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) cmdLine += ' ';
        cmdLine += args[i];
    }
    log("[" + tag + "] FFmpeg command: " + cmdLine);

    pid_t pid = fork();
    if (pid < 0) {
        log("[" + tag + "] FAILED — fork() error");
        return nullptr;
    }
    if (pid == 0) {
        // Child: redirect stdin to /dev/null so FFmpeg doesn't block on it
        int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0) { dup2(devNull, STDIN_FILENO); close(devNull); }

        int logFd = open(m_logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logFd >= 0) {
            dup2(logFd, STDOUT_FILENO);
            dup2(logFd, STDERR_FILENO);
            if (logFd > STDERR_FILENO) close(logFd);
        }

        execvp("ffmpeg", const_cast<char* const*>(argv.data()));
        // execvp only returns on error
        _exit(127);
    }
    log("[" + tag + "] FFmpeg process started (PID " + std::to_string(pid) + ")");
    return reinterpret_cast<void*>(static_cast<intptr_t>(pid));
#endif
}

bool Worker::ffmpegProcAlive(void* h) {
#ifdef _WIN32
    HANDLE hProc = reinterpret_cast<HANDLE>(h);
    if (!hProc || hProc == INVALID_HANDLE_VALUE) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(hProc, &exitCode)) return false;
    if (exitCode == STILL_ACTIVE) return true;
    CloseHandle(hProc);
    return false;
#else
    intptr_t pid = reinterpret_cast<intptr_t>(h);
    if (pid <= 0) return false;
    int status = 0;
    pid_t rc = waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (rc == 0) return true;
    return false;
#endif
}

void Worker::killFfmpegProc(void*& h) {
#ifdef _WIN32
    HANDLE hProc = reinterpret_cast<HANDLE>(h);
    if (hProc && hProc != INVALID_HANDLE_VALUE) {
        TerminateProcess(hProc, 0);
        WaitForSingleObject(hProc, 2000);
        CloseHandle(hProc);
    }
    h = nullptr;
#else
    intptr_t pid = reinterpret_cast<intptr_t>(h);
    if (pid > 0) {
        ::kill(static_cast<pid_t>(pid), SIGTERM);
        // Poll for up to 2 s, then SIGKILL
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            if (waitpid(static_cast<pid_t>(pid), &status, WNOHANG) != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Reap child (non-blocking — may already be reaped above)
        waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
    }
    h = nullptr;
#endif
}

// Helpers: parse Icecast mount URL (http://host:port/mount) into host, port, mount
static void parseIcecastUrl(const std::string& url,
                             std::string& host, int& port, std::string& mount) {
    host  = "localhost";
    port  = 8000;
    mount = "stream";

    std::string rest = url;
    // Strip scheme
    auto schemeEnd = rest.find("://");
    if (schemeEnd != std::string::npos) rest = rest.substr(schemeEnd + 3);

    auto slashPos = rest.find('/');
    std::string hostPort = slashPos != std::string::npos ? rest.substr(0, slashPos) : rest;
    mount = slashPos != std::string::npos ? rest.substr(slashPos + 1) : "stream";

    auto colonPos = hostPort.rfind(':');
    if (colonPos != std::string::npos) {
        host = hostPort.substr(0, colonPos);
        try { port = std::stoi(hostPort.substr(colonPos + 1)); } catch (...) {}
    } else {
        host = hostPort;
    }
}

// ---------- stream lifecycle ----------

void Worker::startAAC() {
    auto cfg = readJsonFile(m_cfgDir + "/aac.json");
    std::string url  = cfg.getString("url",  "");
    std::string user = cfg.getString("user", "source");
    std::string pass = cfg.getString("pass", "hackme");
    int bitrateBps = cfg.getInt("bitrate", 128000);
    int bitrateKbps = clampInt(bitrateBps / 1000, 16, 512);
    int sampleRate = cfg.getInt("sampleRate", 0);
    sampleRate = sampleRate > 0 ? sampleRate : 0;
    std::string mode = lowerCopy(trimCopy(cfg.getString("mode", "legacy")));
    if (mode != "advanced") mode = "legacy";
    std::string profile = lowerCopy(trimCopy(cfg.getString("profile", "aac_low")));
    if (!isAacProfileAllowed(profile)) {
        log("AAC: unsupported profile '" + profile + "' in aac.json; falling back to aac_low");
        profile = "aac_low";
    }

    if (url.empty()) {
        log("AAC: no URL configured in aac.json — cannot start");
        return;
    }

    log("AAC Icecast connecting to: " + url);

    // Build icecast:// URL for FFmpeg
    std::string host, mount;
    int port = 8000;
    parseIcecastUrl(url, host, port, mount);
    std::string icUrl = "icecast://" + user + ":" + pass + "@"
                      + host + ":" + std::to_string(port) + "/" + mount;

    auto inputArgs = buildFfmpegInputArgs();
    std::vector<std::string> args = {"ffmpeg", "-y"};
    args.insert(args.end(), inputArgs.begin(), inputArgs.end());
    args.insert(args.end(), {
        "-vn", "-c:a", "aac", "-b:a", std::to_string(bitrateKbps) + "k"
    });
    if (mode == "advanced") {
        if (sampleRate > 0) {
            args.push_back("-ar");
            args.push_back(std::to_string(sampleRate));
        }
        if (!profile.empty()) {
            args.push_back("-profile:a");
            args.push_back(profile);
        }
    }
    args.insert(args.end(), {
        "-rw_timeout", std::to_string(kIcecastFailureTimeoutUs),
        "-timeout", std::to_string(kIcecastFailureTimeoutUs),
        "-f", "adts", "-content_type", "audio/aac",
        icUrl
    });

    std::ostringstream aacSettings;
    aacSettings << "AAC settings: mode=" << mode
                << " bitrate=" << bitrateKbps << "k";
    if (mode == "advanced") {
        aacSettings << " sampleRate=" << (sampleRate > 0 ? std::to_string(sampleRate) : std::string("auto"))
                    << " profile=" << profile;
    }
    log(aacSettings.str());

    void* h = launchFfmpeg(args, "AAC");
    m_aacProc = h;
    if (!m_aacProc) {
        log("AAC Icecast FAILED — could not start FFmpeg for " + url);
        return;
    }
    log("AAC Icecast stream started successfully — " + url);

    m_aacRunning = true;
    publishStreamHealth();
}

void Worker::stopAAC() {
    killFfmpegProc(m_aacProc);
    m_aacRunning = false;
    log("AAC Icecast stopped");
    publishStreamHealth();
}

void Worker::startMP3() {
    auto cfg = readJsonFile(m_cfgDir + "/mp3.json");
    std::string url  = cfg.getString("url",  "");
    std::string user = cfg.getString("user", "source");
    std::string pass = cfg.getString("pass", "hackme");
    int bitrateBps = cfg.getInt("bitrate", 128000);
    int bitrateKbps = clampInt(bitrateBps / 1000, 32, 320);
    int sampleRate = cfg.getInt("sampleRate", 0);
    sampleRate = sampleRate > 0 ? sampleRate : 0;
    std::string mode = lowerCopy(trimCopy(cfg.getString("mode", "legacy")));
    if (mode != "cbr" && mode != "vbr") mode = "legacy";
    int vbrQuality = clampInt(cfg.getInt("vbrQuality", 4), 0, 9);

    if (url.empty()) {
        log("MP3: no URL configured in mp3.json — cannot start");
        return;
    }

    log("MP3 Icecast connecting to: " + url);

    std::string host, mount;
    int port = 8000;
    parseIcecastUrl(url, host, port, mount);
    std::string icUrl = "icecast://" + user + ":" + pass + "@"
                      + host + ":" + std::to_string(port) + "/" + mount;

    auto inputArgs = buildFfmpegInputArgs();
    std::vector<std::string> args = {"ffmpeg", "-y"};
    args.insert(args.end(), inputArgs.begin(), inputArgs.end());
    args.insert(args.end(), {
        "-vn", "-c:a", "libmp3lame", "-b:a", std::to_string(bitrateKbps) + "k"
    });
    if (mode != "legacy" && sampleRate > 0) {
        args.push_back("-ar");
        args.push_back(std::to_string(sampleRate));
    }
    if (mode == "vbr") {
        // Keep bitrate as a safety cap while letting VBR quality drive target complexity.
        args.push_back("-q:a");
        args.push_back(std::to_string(vbrQuality));
    }
    args.insert(args.end(), {
        "-rw_timeout", std::to_string(kIcecastFailureTimeoutUs),
        "-timeout", std::to_string(kIcecastFailureTimeoutUs),
        "-f", "mp3",
        icUrl
    });

    std::ostringstream mp3Settings;
    mp3Settings << "MP3 settings: mode=" << mode
                << " bitrate=" << bitrateKbps << "k";
    if (mode != "legacy") {
        mp3Settings << " sampleRate=" << (sampleRate > 0 ? std::to_string(sampleRate) : std::string("auto"));
    }
    if (mode == "vbr") mp3Settings << " q=" << vbrQuality;
    log(mp3Settings.str());

    void* h = launchFfmpeg(args, "MP3");
    m_mp3Proc = h;
    if (!m_mp3Proc) {
        log("MP3 Icecast FAILED — could not start FFmpeg for " + url);
        return;
    }
    log("MP3 Icecast stream started successfully — " + url);

    m_mp3Running = true;
    publishStreamHealth();
}

void Worker::stopMP3() {
    killFfmpegProc(m_mp3Proc);
    m_mp3Running = false;
    log("MP3 Icecast stopped");
    publishStreamHealth();
}

// Resolve a Unix-style /etc/... path to an absolute Windows path.
// On Windows the worker sets CWD to C:\ so /etc/ -> C:\etc\ via _fullpath.
// We must call _fullpath on the plain directory (no % patterns).
static std::string resolveWinPath(const std::string& unixPath) {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    if (_fullpath(buf, unixPath.c_str(), MAX_PATH)) return buf;
#endif
    return unixPath;
}

void Worker::startHLS() {
    if (m_hlsRunning.load() || m_hlsProc != nullptr) {
        log("HLS already running; StartHLS ignored");
        return;
    }

    // Determine HLS output directory and resolve to absolute Windows path
    std::string hlsDir = resolveWinPath(m_cfgDir + "/hls");
    // Normalise separators: replace any forward slashes with backslashes on Win32
#ifdef _WIN32
    for (char& c : hlsDir) if (c == '/') c = '\\';
#endif
    fs::create_directories(hlsDir);
    std::string segDir = hlsDir +
#ifdef _WIN32
        "\\segments";
#else
        "/segments";
#endif
    fs::create_directories(segDir);

    // Remove stale m3u8 and old segment files so a fresh stream starts cleanly
    {
        auto m3u8Stale = hlsDir +
#ifdef _WIN32
            "\\index.m3u8";
#else
            "/index.m3u8";
#endif
        std::error_code ec;
        if (fs::exists(m3u8Stale, ec)) {
            fs::remove(m3u8Stale, ec);
            log("HLS: removed stale playlist");
        }
        for (auto& entry : fs::recursive_directory_iterator(hlsDir, ec)) {
            auto ext = entry.path().extension().string();
            if (ext == ".ts" || ext == ".aac" || ext == ".m4s") {
                fs::remove(entry.path(), ec);
            }
        }
    }

    // Load per-encoder admin config for playback port and listen link
    auto adminCfg = readJsonFile(m_cfgDir + "/encoder_admin.json");
    std::string adminListenLink = adminCfg.getString("hlsListenLink",   "");
    int         playbackPort    = adminCfg.getInt   ("hlsPlaybackPort", 0);

    // Resolve the machine hostname for URL construction
    char hostnameBuf[256] = {};
    gethostname(hostnameBuf, sizeof(hostnameBuf) - 1);
    std::string hostname = hostnameBuf[0] ? hostnameBuf : "localhost";

    // Canonical "live stream" URL — built from the playback port when configured,
    // otherwise fall back to the supervisor's built-in HLS proxy route.
    std::string hlsUrl;
    if (playbackPort > 0) {
        hlsUrl = "http://" + hostname + ":" + std::to_string(playbackPort) + "/hls/index.m3u8";
    } else if (!adminListenLink.empty()) {
        hlsUrl = adminListenLink;
    } else {
        hlsUrl = "http://" + hostname + ":8050/encoder/" + std::to_string(m_idx) + "/hls/index.m3u8";
    }

    log("HLS output directory: " + hlsDir);
    log("HLS stream URL:       " + hlsUrl);

    auto cfg = readJsonFile(m_cfgDir + "/hls.json");
    int segSecs  = cfg.getInt("segmentSeconds", 5);
    int window   = cfg.getInt("window", 5);

    auto inputArgs = buildFfmpegInputArgs();
    // Use backslash separator on Windows so FFmpeg resolves the output paths
    // without needing Unix path emulation.
    char sep =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif
    std::string segPattern = segDir + sep + "segment-%d.aac";
    std::string m3u8Path   = hlsDir + sep + "index.m3u8";
    log("HLS segment pattern:  " + segPattern);

    std::vector<std::string> args = {"ffmpeg", "-y"};
    args.insert(args.end(), inputArgs.begin(), inputArgs.end());
    args.insert(args.end(), {
        "-vn", "-c:a", "aac", "-b:a", "128k", "-ac", "2",
        "-f", "segment",
        "-segment_time",         std::to_string(segSecs),
        "-segment_list_size",    std::to_string(window),
        "-segment_list_flags",   "+live",
        "-segment_list_type",    "m3u8",
        "-segment_list_entry_prefix", "segments/",
        "-segment_format",       "adts",
        "-segment_list",         m3u8Path,
        segPattern
    });

    void* h = launchFfmpeg(args, "HLS");
    m_hlsProc = h;
    if (!m_hlsProc) {
        log("HLS FAILED — could not start FFmpeg");
        return;
    }
    log("HLS FFmpeg encoding started");

    // Start segment monitor — logs each .ts file as FFmpeg writes it
    if (m_hlsSegMonThread.joinable()) {
        m_hlsSegMonRunning = false;
        m_hlsSegMonThread.join();
    }
    m_hlsSegMonRunning = true;
    m_hlsSegMonThread = std::thread([this, hlsDir]() { monitorHlsSegments(hlsDir); });

    // Start dedicated HLS HTTP server on the configured playback port
    if (playbackPort > 0) {
        if (m_hlsHttpThread.joinable()) {
            // A previous server thread is still alive — stop it first
            m_hlsHttpRunning = false;
            int old = m_hlsHttpSocket.load();
            if (old != -1) close_socket(old);
            m_hlsHttpThread.join();
        }
        m_hlsHttpRunning = true;
        m_hlsHttpThread = std::thread([this, playbackPort, hlsDir]() {
            serveHlsHttp(playbackPort, hlsDir);
        });
#ifdef _WIN32
        // Ensure Windows Firewall allows inbound traffic on the playback port.
        // Requires elevation; if the process is not running as Administrator
        // the command will fail silently. Use windows-install.ps1 (as Admin)
        // to add rules permanently, or run the supervisor as Administrator.
        {
            std::string portStr = std::to_string(playbackPort);
            std::string ruleName = "MultiCoder HLS Port " + portStr;
            std::string netshCmd = "netsh advfirewall firewall add rule "
                "name=\"" + ruleName + "\" protocol=TCP dir=in localport=" + portStr
                + " action=allow >nul 2>&1";
            log("HLS: ensuring firewall inbound rule for TCP port " + portStr
                + " (requires elevation \u2014 run scripts/windows-install.ps1 as Admin if this fails)");
            ::system(netshCmd.c_str());
        }
#endif
        log("HLS HTTP server listening on port " + std::to_string(playbackPort)
            + " — access stream at: " + hlsUrl);
    } else {
        log("HLS stream access URL: " + hlsUrl);
    }

    m_hlsRunning = true;
    publishStreamHealth();
}

void Worker::stopHLS() {
    // Stop segment monitor
    if (m_hlsSegMonRunning.load()) {
        m_hlsSegMonRunning = false;
        if (m_hlsSegMonThread.joinable()) m_hlsSegMonThread.join();
    }
    // Stop HLS HTTP server
    if (m_hlsHttpRunning.load()) {
        m_hlsHttpRunning = false;
        int s = m_hlsHttpSocket.load();
        if (s != -1) close_socket(s);
        if (m_hlsHttpThread.joinable()) m_hlsHttpThread.join();
    }
    killFfmpegProc(m_hlsProc);
    m_hlsRunning = false;
    log("HLS stopped");
    publishStreamHealth();
}

void Worker::startSRT() {
    if (m_srtRunning.load() || m_srtProc != nullptr) {
        log("SRT already running; StartSRT ignored");
        return;
    }

    auto cfg = readJsonFile(m_cfgDir + "/srt.json");
    std::string transport = lowerCopy(trimCopy(cfg.getString("transport", "mpeg-ts")));
    std::string mode = lowerCopy(trimCopy(cfg.getString("mode", "caller")));
    std::string host = trimCopy(cfg.getString("host", ""));
    int port = cfg.getInt("port", 9150);
    std::string streamId = trimCopy(cfg.getString("streamId", ""));
    int latency = cfg.getInt("latency", 120);
    int buffer = cfg.getInt("buffer", 1024);
    std::string encryption = lowerCopy(trimCopy(cfg.getString("encryption", "none")));
    std::string passphrase = cfg.getString("passphrase", "");
    int pbkeylen = cfg.getInt("pbkeylen", 16);

    if (host.empty()) {
        log("SRT FAILED — host is empty in srt.json");
        return;
    }
    if (port <= 0 || port > 65535) {
        log("SRT FAILED — invalid port in srt.json: " + std::to_string(port));
        return;
    }
    if (mode.empty()) mode = "caller";
    if (transport.empty()) transport = "mpeg-ts";

    if (transport != "mpeg-ts" && transport != "mpegts") {
        log("SRT FAILED — unsupported transport '" + transport + "'. Only MPEG-TS is currently implemented.");
        return;
    }

    auto inputArgs = buildFfmpegInputArgs();
    std::string uri = "srt://" + host + ":" + std::to_string(port) + "?mode=" + mode;
    if (latency > 0) uri += "&latency=" + std::to_string(latency);
    if (buffer > 0) uri += "&sndbuf=" + std::to_string(buffer * 1024);
    if (!streamId.empty()) uri += "&streamid=" + streamId;
    if (!passphrase.empty()) {
        uri += "&passphrase=" + passphrase;
        if (pbkeylen == 16 || pbkeylen == 24 || pbkeylen == 32) {
            uri += "&pbkeylen=" + std::to_string(pbkeylen);
        }
    }

    log("SRT preparing connection");
    log("SRT transport: MPEG-TS");
    log("SRT mode: " + mode);
    log("SRT destination: " + host + ":" + std::to_string(port));
    log("SRT latency: " + std::to_string(latency) + " ms");
    log("SRT buffer: " + std::to_string(buffer) + " KB");
    log("SRT streamId: " + (streamId.empty() ? std::string("(none)") : streamId));
    log("SRT encryption: " + encryption);
    log("SRT passphrase configured: " + std::string(passphrase.empty() ? "no" : "yes"));
    log("SRT output URI: " + maskSrtUriPassphrase(uri));

    std::vector<std::string> args = {"ffmpeg", "-y", "-loglevel", "debug"};
    args.insert(args.end(), inputArgs.begin(), inputArgs.end());
    args.insert(args.end(), {
        "-vn", "-c:a", "aac", "-b:a", "128k", "-ac", "2",
        "-f", "mpegts",
        uri
    });

    void* h = launchFfmpeg(args, "SRT");
    m_srtProc = h;
    if (!m_srtProc) {
        log("SRT FAILED — could not start FFmpeg for " + host + ":" + std::to_string(port));
        return;
    }

    log("SRT FFmpeg stream started — waiting for connection/handshake details in encoder log");
    m_srtRunning = true;
    publishStreamHealth();
}

void Worker::stopSRT() {
    killFfmpegProc(m_srtProc);
    m_srtRunning = false;
    log("SRT stopped");
    publishStreamHealth();
}



// ---------- HLS segment monitor ----------
// Polls the HLS output directory and logs each new .ts segment as it appears.

void Worker::monitorHlsSegments(const std::string& hlsDir) {
    std::set<std::string> seen;
    int segCount = 0;

    // Pre-populate seen set so we only log NEW segments written after start
    std::error_code ec;
    for (auto& e : fs::directory_iterator(hlsDir, ec)) {
        auto ext = e.path().extension().string();
        if (ext == ".ts" || ext == ".aac")
            seen.insert(e.path().filename().string());
    }

    while (m_hlsSegMonRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!m_hlsSegMonRunning.load()) break;

        for (auto& entry : fs::directory_iterator(hlsDir, ec)) {
            auto ext = entry.path().extension().string();
            if (ext != ".ts" && ext != ".aac") continue;
            std::string fname = entry.path().filename().string();
            if (seen.count(fname)) continue;
            seen.insert(fname);
            ++segCount;
            auto sz = entry.file_size(ec);
            log("HLS segment #" + std::to_string(segCount) + " written: " + fname
                + " (" + std::to_string(sz / 1024) + " KB)");
        }

        // Prune seen set to avoid unbounded growth — keep only files still on disk
        if (segCount % 20 == 0 && segCount > 0) {
            std::set<std::string> stillPresent;
            for (auto& e : fs::directory_iterator(hlsDir, ec)) {
                auto ext = e.path().extension().string();
                if (ext == ".ts" || ext == ".aac")
                    stillPresent.insert(e.path().filename().string());
            }
            seen = stillPresent;
        }
    }
}

// Convert an ISO-8601 datetime string with a local-time offset to UTC Z format.
// e.g. "2026-04-10T23:48:04.346-0400" -> "2026-04-11T03:48:04.346Z"
// Returns the input unchanged if already UTC (ends 'Z') or unparseable.
static std::string isoToUtcZ(const std::string& s) {
    if (s.empty() || s.back() == 'Z') return s;
    size_t tPos = s.find('T');
    if (tPos == std::string::npos || s.size() < 20) return s;
    // Find timezone sign (first + or - that appears *after* the seconds digits)
    size_t signPos = std::string::npos;
    for (size_t i = tPos + 7; i < s.size(); ++i) {
        if (s[i] == '+' || s[i] == '-') { signPos = i; break; }
    }
    if (signPos == std::string::npos) return s;
    try {
        const std::string base = s.substr(0, signPos);
        const std::string tz   = s.substr(signPos);
        if (base.size() < 19) return s;
        int yr = std::stoi(base.substr(0, 4));
        int mo = std::stoi(base.substr(5, 2));
        int dy = std::stoi(base.substr(8, 2));
        int hr = std::stoi(base.substr(11, 2));
        int mn = std::stoi(base.substr(14, 2));
        int sc = std::stoi(base.substr(17, 2));
        std::string frac = (base.size() > 19 && base[19] == '.') ? base.substr(19) : "";
        // Parse offset ±HHMM or ±HH:MM
        int tzSign = (tz[0] == '-') ? -1 : 1;
        std::string tzd = tz.substr(1);
        tzd.erase(std::remove(tzd.begin(), tzd.end(), ':'), tzd.end());
        if (tzd.size() < 4) return s;
        int offsetSecs = tzSign * (std::stoi(tzd.substr(0, 2)) * 3600
                                 + std::stoi(tzd.substr(2, 2)) * 60);
        if (offsetSecs == 0) return base + frac + "Z";
        std::tm tm{};
        tm.tm_year = yr - 1900; tm.tm_mon = mo - 1; tm.tm_mday = dy;
        tm.tm_hour = hr; tm.tm_min = mn; tm.tm_sec = sc;
#ifdef _WIN32
        time_t epoch = _mkgmtime(&tm);
#else
        time_t epoch = timegm(&tm);
#endif
        if (epoch == (time_t)-1) return s;
        epoch -= offsetSecs; // subtract local offset to get UTC
        std::tm utc{};
#ifdef _WIN32
        if (gmtime_s(&utc, &epoch) != 0) return s;
#else
        if (!gmtime_r(&epoch, &utc)) return s;
#endif
        char buf[24];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &utc);
        return std::string(buf) + frac + "Z";
    } catch (...) { return s; }
}

static std::string formatUtcIso8601(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch());
    auto secs = duration_cast<seconds>(ms);
    auto millis = ms - secs;
    std::time_t tt = secs.count();
    std::tm gmt{};
#ifdef _WIN32
    gmtime_s(&gmt, &tt);
#else
    gmtime_r(&tt, &gmt);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &gmt);
    std::ostringstream oss;
    oss << buf << "." << std::setw(3) << std::setfill('0') << millis.count() << "Z";
    return oss.str();
}

// ---------- HLS HTTP playback server ----------
// Minimal HTTP/1.0 server that serves the HLS playlist and segment files
// from hlsDir on the configured playback port.

void Worker::serveHlsHttp(int port, const std::string& hlsDir) {
    initSocketsOnce();
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        log("HLS HTTP server: socket() failed");
        m_hlsHttpRunning = false;
        return;
    }
    int opt = 1;
#ifdef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log("HLS HTTP server: bind() failed on port " + std::to_string(port)
            + " — check that port is not already in use");
        close_socket(sfd);
        m_hlsHttpRunning = false;
        return;
    }
    listen(sfd, 8);
    m_hlsHttpSocket = sfd;
    log("HLS HTTP server ready on port " + std::to_string(port));

    while (m_hlsHttpRunning.load()) {
        // Non-blocking accept with 500 ms poll so shutdown is responsive
        fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
        timeval tv{0, 500000};
        int sel = select(sfd + 1, &fds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in clientAddr{};
        socklen_t   clientLen = sizeof(clientAddr);
        int cfd = accept(sfd, (sockaddr*)&clientAddr, &clientLen);
        if (cfd < 0) continue;

        // Read the request line (we need only the path)
        char buf[2048] = {};
        recv(cfd, buf, sizeof(buf) - 1, 0);
        std::string reqStr(buf);

        // Parse: GET /hls/index.m3u8 HTTP/...
        std::string reqPath;
        {
            std::istringstream iss(reqStr);
            std::string method, rawPath;
            if ((iss >> method >> rawPath) && method == "GET") {
                reqPath = rawPath;
            }
        }

        auto sendResp = [&](int code, const std::string& ct, const std::string& body) {
            const char* status = code == 200 ? "OK" : "Not Found";
            const char* cacheHdr = (ct.find("mpegurl") != std::string::npos)
                ? "Cache-Control: no-cache, no-store\r\nPragma: no-cache\r\n"
                : "Cache-Control: max-age=600\r\n";
            char hdr[512];
            snprintf(hdr, sizeof(hdr),
                "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                "Access-Control-Allow-Origin: *\r\n%sConnection: close\r\n\r\n",
                code, status, ct.c_str(), body.size(), cacheHdr);
            std::string resp = std::string(hdr) + body;
            send(cfd, resp.c_str(), (int)resp.size(), MSG_NOSIGNAL);
        };

        // Validate: only serve /hls/ files, no path traversal
        bool bad = reqPath.empty()
                || reqPath.find("..") != std::string::npos
                || reqPath.rfind("/hls/", 0) != 0;
        if (bad || reqPath == "/hls/") {
            // Redirect bare /hls/ to /hls/index.m3u8
            if (reqPath == "/hls/" || reqPath == "/hls") {
                std::string redir = "HTTP/1.0 302 Found\r\nLocation: /hls/index.m3u8\r\n"
                                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                send(cfd, redir.c_str(), (int)redir.size(), MSG_NOSIGNAL);
            } else {
                sendResp(404, "text/plain", "Not Found");
            }
            close_socket(cfd);
            continue;
        }

        std::string filename = reqPath.substr(5); // strip leading "/hls/"
        // Allow only:
        //   - "index.m3u8"
        //   - "segment-*.aac"
        //   - "segments/segment-*.aac"
        // Block traversal and any other nested paths.
        bool badName = filename.empty() ||
                       filename.find("..") != std::string::npos ||
                       filename.find('\\') != std::string::npos;
        size_t slashPos = filename.find('/');
        if (!badName && slashPos != std::string::npos) {
            bool allowedSegPath = filename.rfind("segments/", 0) == 0 &&
                                  filename.find('/', 9) == std::string::npos;
            if (!allowedSegPath) badName = true;
        }
        if (badName) {
            sendResp(404, "text/plain", "Not Found");
            close_socket(cfd);
            continue;
        }

        std::string filePath = hlsDir + "/" + filename;
        std::string data = readTextFile(filePath);
        if (data.empty() && !fs::exists(filePath)) {
            sendResp(404, "text/plain", "Segment not found");
            close_socket(cfd);
            continue;
        }

        bool isM3u8 = filename.size() >= 5 &&
                      filename.compare(filename.size() - 5, 5, ".m3u8") == 0;
        if (isM3u8 && !data.empty()) {
            // Enrich the playlist in-memory so FFmpeg's periodic overwrites don't wipe
            // our changes.  Fixes:
            //   1. EXT-X-VERSION upgraded to 6 (required for per-segment PROGRAM-DATE-TIME)
            //   2. EXT-X-INDEPENDENT-SEGMENTS added (AAC-only stream requirement)
            //   3. EXT-X-START:TIME-OFFSET added (tells player where to begin)
            //   4. EXT-X-PROGRAM-DATE-TIME moved from AFTER EXTINF to BEFORE it
            //      (FFmpeg emits wrong order; Orban/clients require it before EXTINF)
            //   5. Current metadata payload injected onto every EXTINF line

            // 1. Grab current metadata payload.
            std::string metaPayload;
            {
                std::lock_guard<std::mutex> lk(m_hlsMetaMutex);
                metaPayload = m_hlsLastMetaPayload;
            }
            if (metaPayload.empty()) {
                simplejson::Object mrt = readJsonFile(m_cfgDir + "/metadata_runtime.json");
                metaPayload = mrt.getString("lastFormattedHLS", "");
            }
            if (!isSafeMetaForExtinf(metaPayload)) metaPayload = sanitizeForExtinf(metaPayload);

            // 2. Read HLS config for startTimeOffset and segment cadence.
            int startOffset = -25;
            int cfgSegSecs = 6;
            {
                simplejson::Object hlsCfgLocal = readJsonFile(m_cfgDir + "/hls.json");
                startOffset = hlsCfgLocal.getInt("startTimeOffset", -25);
                cfgSegSecs = hlsCfgLocal.getInt("segmentSeconds", 6);
            }
            if (cfgSegSecs <= 0) cfgSegSecs = 6;
            std::ostringstream durOs;
            durOs << std::fixed << std::setprecision(3) << static_cast<double>(cfgSegSecs);
            const std::string fixedDur = durOs.str();

            // 3. Parse lines.
            std::vector<std::string> plines;
            {
                std::istringstream iss(data);
                std::string ln;
                while (std::getline(iss, ln)) {
                    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                    plines.push_back(ln);
                }
            }

            // 4. Upgrade version, track insertion point.
            bool hasIndep = false, hasStart = false;
            size_t mediaSeqIdx = std::string::npos;
            for (size_t i = 0; i < plines.size(); ++i) {
                if (plines[i].rfind("#EXT-X-VERSION:", 0) == 0)    plines[i] = "#EXT-X-VERSION:6";
                if (plines[i].rfind("#EXT-X-TARGETDURATION:", 0) == 0)
                    plines[i] = "#EXT-X-TARGETDURATION:" + std::to_string(cfgSegSecs);
                if (plines[i] == "#EXT-X-ALLOW-CACHE:YES") plines[i].clear();
                if (plines[i] == "#EXT-X-INDEPENDENT-SEGMENTS")     hasIndep  = true;
                if (plines[i].rfind("#EXT-X-START:", 0) == 0)      hasStart  = true;
                if (plines[i].rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) mediaSeqIdx = i;
            }

            // 5. Insert missing header tags after EXT-X-MEDIA-SEQUENCE.
            if (mediaSeqIdx != std::string::npos) {
                std::vector<std::string> ins;
                if (!hasIndep)  ins.push_back("#EXT-X-INDEPENDENT-SEGMENTS");
                if (!hasStart)  ins.push_back(
                    "#EXT-X-START:TIME-OFFSET=" + std::to_string(startOffset) + ",PRECISE=YES");
                if (!ins.empty())
                    plines.insert(plines.begin() + (long)mediaSeqIdx + 1, ins.begin(), ins.end());
            }

            // 6a. Normalize all #EXT-X-PROGRAM-DATE-TIME timestamps to UTC Z format.
            //     FFmpeg emits local time with offset (e.g. -0400); Orban requires UTC.
            bool hasAnyPdt = false;
            for (auto& ln : plines) {
                if (ln.rfind("#EXT-X-PROGRAM-DATE-TIME:", 0) == 0) {
                    hasAnyPdt = true;
                    ln = "#EXT-X-PROGRAM-DATE-TIME:" + isoToUtcZ(ln.substr(25));
                }
            }

            // If muxer omitted PROGRAM-DATE-TIME, synthesize one per EXTINF.
            if (!hasAnyPdt) {
                std::vector<size_t> extIdx;
                std::vector<double> extDur;
                std::vector<int> extSeq;
                for (size_t i = 0; i < plines.size(); ++i) {
                    if (plines[i].rfind("#EXTINF:", 0) != 0) continue;
                    size_t comma = plines[i].find(',');
                    std::string ds = (comma == std::string::npos)
                        ? plines[i].substr(8)
                        : plines[i].substr(8, comma - 8);
                    (void)ds;
                    double d = static_cast<double>(cfgSegSecs);
                    extIdx.push_back(i);
                    extDur.push_back(d);

                    int seq = -1;
                    if (i + 1 < plines.size()) {
                        const std::string& seg = plines[i + 1];
                        size_t p = seg.rfind("segment-");
                        if (p != std::string::npos) {
                            p += 8;
                            size_t q = p;
                            while (q < seg.size() && std::isdigit(static_cast<unsigned char>(seg[q]))) q++;
                            if (q > p) {
                                try { seq = std::stoi(seg.substr(p, q - p)); } catch (...) { seq = -1; }
                            }
                        }
                    }
                    extSeq.push_back(seq);
                }
                if (!extIdx.empty()) {
                    static std::mutex s_pdtAnchorMutex;
                    static bool s_anchorInit = false;
                    static int s_anchorSeq = 0;
                    static double s_anchorEpoch = 0.0; // seconds, UTC epoch, at anchor seq start
                    static double s_anchorStep = 6.0;  // nominal seconds per segment

                    double firstDur = static_cast<double>(cfgSegSecs);
                    int firstSeq = extSeq[0];
                    double firstEpoch = 0.0;
                    {
                        std::lock_guard<std::mutex> lk(s_pdtAnchorMutex);
                        if (!s_anchorInit || firstSeq < 0 || firstSeq < s_anchorSeq - 50) {
                            // Initial anchor (or stream reset): place first visible segment in the recent past.
                            double nowEpoch = std::chrono::duration<double>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            s_anchorSeq = (firstSeq >= 0) ? firstSeq : 0;
                            s_anchorStep = firstDur;
                            s_anchorEpoch = nowEpoch - 0.5 * s_anchorStep;
                            s_anchorInit = true;
                        } else {
                            // Advance anchor as sequence moves forward to keep mapping stable.
                            if (firstSeq >= 0) {
                                if (firstSeq > s_anchorSeq) {
                                    s_anchorEpoch += (firstSeq - s_anchorSeq) * s_anchorStep;
                                    s_anchorSeq = firstSeq;
                                }
                                s_anchorStep = static_cast<double>(cfgSegSecs);
                            }
                        }
                        if (firstSeq >= 0) firstEpoch = s_anchorEpoch;
                        else {
                            double nowEpoch = std::chrono::duration<double>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            firstEpoch = nowEpoch - 0.5 * firstDur;
                        }
                    }

                    auto epochToTp = [](double sec) {
                        return std::chrono::system_clock::time_point(
                            std::chrono::milliseconds(static_cast<long long>(std::llround(sec * 1000.0))));
                    };

                    size_t inserted = 0;
                    double curEpoch = firstEpoch;
                    for (size_t k = 0; k < extIdx.size(); ++k) {
                        size_t at = extIdx[k] + inserted;
                        plines.insert(plines.begin() + static_cast<long long>(at),
                            "#EXT-X-PROGRAM-DATE-TIME:" + formatUtcIso8601(epochToTp(curEpoch)));
                        inserted++;
                        curEpoch += static_cast<double>(cfgSegSecs);
                    }
                }
            }

            // 6b. Fix ordering + inject metadata.
            //    FFmpeg emits:  #EXTINF  then  #EXT-X-PROGRAM-DATE-TIME  then  seg.ts
            //    Spec requires: #EXT-X-PROGRAM-DATE-TIME  then  #EXTINF  then  seg.ts
            for (size_t i = 0; i < plines.size(); ++i) {
                if (plines[i].rfind("#EXTINF:", 0) == 0) {
                    if (i + 1 < plines.size() &&
                        plines[i + 1].rfind("#EXT-X-PROGRAM-DATE-TIME:", 0) == 0) {
                        std::swap(plines[i], plines[i + 1]);
                        ++i; // plines[i] is now PROGRAM-DATE-TIME; advance to EXTINF
                    }
                    // Normalize EXTINF duration and inject metadata payload.
                    plines[i] = "#EXTINF:" + fixedDur + ",";
                    if (!metaPayload.empty()) plines[i] += metaPayload;
                }
            }

            // 6c. Match original header ordering:
            //     #EXT-X-VERSION
            //     #EXT-X-TARGETDURATION
            //     #EXT-X-MEDIA-SEQUENCE
            size_t idxVer = std::string::npos, idxTd = std::string::npos, idxMs = std::string::npos;
            for (size_t i = 0; i < plines.size(); ++i) {
                if (plines[i].rfind("#EXT-X-VERSION:", 0) == 0) idxVer = i;
                else if (plines[i].rfind("#EXT-X-TARGETDURATION:", 0) == 0) idxTd = i;
                else if (plines[i].rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) idxMs = i;
            }
            if (idxVer != std::string::npos && idxTd != std::string::npos && idxMs != std::string::npos) {
                std::string tdLine = plines[idxTd];
                plines.erase(plines.begin() + static_cast<long long>(idxTd));
                if (idxTd < idxMs) idxMs--;
                plines.insert(plines.begin() + static_cast<long long>(idxVer + 1), tdLine);
            }

            // 7. Rebuild.
            std::ostringstream rebuilt;
            for (const auto& ln : plines) {
                if (ln.empty()) continue;
                rebuilt << ln << "\n";
            }
            data = rebuilt.str();
        }

        bool isAac = filename.size() >= 4 &&
                     filename.compare(filename.size() - 4, 4, ".aac") == 0;
        std::string ct = isM3u8 ? "audio/mpegurl" : (isAac ? "audio/aac" : "video/mp2t");
        sendResp(200, ct, data);
        close_socket(cfd);
    }

    close_socket(sfd);
    m_hlsHttpSocket = -1;
    log("HLS HTTP server stopped");
}

// ---------- control port listener ----------

void Worker::listenControlPort() {
    // Read control port from /etc/encoder{N}/control.json
    // Default: 9100 + (encoderIdx - 1) * 10
    int ctlPort = 9100 + (m_idx - 1) * 10;
    // Try to read from config file
    {
        std::ifstream cf(m_cfgDir + "/control.json");
        if (cf.is_open()) {
            std::string s((std::istreambuf_iterator<char>(cf)), {});
            simplejson::Object o;
            if (o.parse(s)) {
                ctlPort = o.getInt("controlPort", ctlPort);
            }
        }
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { log("control socket() failed"); return; }
    int opt = 1;
#ifdef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(ctlPort));
    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log("control bind() failed on port " + std::to_string(ctlPort));
        close_socket(sfd);
        return;
    }
    listen(sfd, 8);
    log("Control listener on port " + std::to_string(ctlPort));
    m_controlListenerRunning = true;
    publishStreamHealth();

    while (m_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
        timeval tv{1, 0};
        if (select(sfd+1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in cli{}; socklen_t len = sizeof(cli);
        int cfd = accept(sfd, (sockaddr*)&cli, &len);
        if (cfd < 0) continue;
        char buf[256] = {};
        ssize_t n = recv(cfd, buf, sizeof(buf)-1, 0);
        if (n > 0) {
            std::string cmd(buf, n);
            // Trim whitespace
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' '))
                cmd.pop_back();
            log("Control command: " + cmd);

            bool known = (cmd == "StartAAC" || cmd == "StopAAC" ||
                          cmd == "StartMP3" || cmd == "StopMP3" ||
                          cmd == "StartHLS" || cmd == "StopHLS" ||
                          cmd == "StartSRT" || cmd == "StopSRT");

            if (known) {
                // ACK first so supervisor retries don't duplicate long-running start operations.
                const char* ack = "OK\n";
                send(cfd, ack, 3, MSG_NOSIGNAL);

                if      (cmd == "StartAAC")  startAAC();
                else if (cmd == "StopAAC")   stopAAC();
                else if (cmd == "StartMP3")  startMP3();
                else if (cmd == "StopMP3")   stopMP3();
                else if (cmd == "StartHLS")  startHLS();
                else if (cmd == "StopHLS")   stopHLS();
                else if (cmd == "StartSRT")  startSRT();
                else if (cmd == "StopSRT")   stopSRT();
            } else {
                log("Unknown control command: " + cmd);
                const char* nack = "ERR\n";
                send(cfd, nack, 4, MSG_NOSIGNAL);
            }
        }
        close_socket(cfd);
    }
    close_socket(sfd);
    m_controlListenerRunning = false;
    publishStreamHealth();
}

void Worker::monitorInputLevels() {
    int levelSocket = -1;
    std::string activeAddr;
    int activePort = 0;
    std::string activeIface;
    std::string activeInputType;
    std::string activeAudioDevice;
    int activeSampleRate = 48000;
#ifdef _WIN32
    WaveInputMeter waveMeter;
    WasapiInputMeter wasapiMeter;
#endif
    bool lastConnected = false;
    std::string lastFailure;
    std::string lastUnsupportedMode;
    auto lastDataAt = std::chrono::steady_clock::now();
    auto lastNoDataLogAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastMeterUpdateAt = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    int lastMeterL = -60;
    int lastMeterR = -60;
    auto calibrateMeterDb = [](int rawDb, double gainDb) {
        // VU should reflect user gain directly. Allow a +12 dB display headroom
        // so changes above 0 dB remain visible in the UI.
        int displayDb = static_cast<int>(std::lround(static_cast<double>(rawDb) + gainDb));
        return clampMeterDb(displayDb);
    };
    auto stampHealth = [this](simplejson::Object& rt) {
        rt.setInt("workerHeartbeatEpoch", static_cast<int>(std::time(nullptr)));
        rt.setBool("workerAacRunning", m_aacRunning.load());
        rt.setBool("workerMp3Running", m_mp3Running.load());
        rt.setBool("workerHlsRunning", m_hlsRunning.load());
        rt.setBool("workerSrtRunning", m_srtRunning.load());
        rt.setBool("controlListenerRunning", m_controlListenerRunning.load());
    };

#ifdef _WIN32
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif

    while (m_running && !g_workerShutdown) {
        // Read session config (no lock needed - supervisor/UI writes, worker only reads here)
        simplejson::Object cfg = readRuntimeState(m_cfgDir);
        bool connected = cfg.getBool("inputConnected", false);

        simplejson::Object inCfg = readJsonFile(m_cfgDir + "/input.json");
        std::string inType = cfg.getString("sessionInputType", inCfg.getString("inputType", "rtp"));
        std::string rtpAddr = cfg.getString("sessionRtpAddress", inCfg.getString("rtpAddress", ""));
        int rtpPort = cfg.getInt("sessionRtpPort", inCfg.getInt("rtpPort", 5004));
        double gainDb = cfg.getDouble("sessionGainDb", inCfg.getDouble("rtpGain", 0.0));
        std::string iface = cfg.getString("sessionRtpInterface", inCfg.getString("rtpInterface", ""));
        std::string audioDevice = cfg.getString("sessionAudioDevice", inCfg.getString("audioDevice", ""));
        int sampleRate = cfg.getInt("sessionSampleRate", inCfg.getInt("sampleRate", 48000));
        int bitDepth   = inCfg.getInt("bitDepth", 24);

        if (connected && !lastConnected) {
            log("Input connect requested: type=" + inType + " addr=" + rtpAddr +
                " port=" + std::to_string(rtpPort) +
                (iface.empty() ? "" : " iface=" + iface) +
                " gain=" + std::to_string(gainDb) + "dB");
        }
        if (!connected && lastConnected) {
            log("Input disconnected by UI");
        }
        lastConnected = connected;

        bool canSampleRtp = connected && (inType == "rtp" || inType == "axia") && !rtpAddr.empty() && rtpPort > 0;
        bool canSampleAudio = connected && (inType == "audio");

        if ((inType != activeInputType) && levelSocket >= 0) {
            close_socket(levelSocket);
            levelSocket = -1;
            activeAddr.clear();
            activePort = 0;
            activeIface.clear();
        }
#ifdef _WIN32
        if ((inType != activeInputType) && waveMeter.open) {
            closeWaveInput(waveMeter);
            activeAudioDevice.clear();
            activeSampleRate = 48000;
        }
    if ((inType != activeInputType) && wasapiMeter.open) {
        closeWasapiInput(wasapiMeter);
    }
#endif
        activeInputType = inType;

        if (!canSampleRtp) {
            if (levelSocket >= 0) {
                close_socket(levelSocket);
                levelSocket = -1;
                activeAddr.clear();
                activePort = 0;
                activeIface.clear();
            }

#ifdef _WIN32
            if (canSampleAudio) {
                UINT devId = WAVE_MAPPER;
                std::string mode = std::string("audio:") + (audioDevice.empty() ? "default" : audioDevice) + ":" + std::to_string(sampleRate);
                bool validDevId = parseWaveDeviceId(audioDevice, devId);
                if (!validDevId && !audioDevice.empty()) {
                    // Token is not numeric — try matching by device name (stable across reboots)
                    validDevId = findWaveDeviceByName(audioDevice, devId);
                }
                if (!validDevId && !audioDevice.empty()) {
                    if (lastUnsupportedMode != mode) {
                        log("Audio input connect failed: invalid audio device id '" + audioDevice + "'");
                        lastUnsupportedMode = mode;
                    }
                } else if (!waveMeter.open || activeAudioDevice != audioDevice || activeSampleRate != sampleRate) {
                    std::string waveErr;
                    if (!openWaveInput(waveMeter, devId, sampleRate, waveErr)) {
                        if (lastUnsupportedMode != mode) {
                            log("Audio input connect failed (waveIn): " + waveErr + " — will attempt WASAPI");
                            lastUnsupportedMode = mode;
                        }
                    } else {
                        activeAudioDevice = audioDevice;
                        activeSampleRate = sampleRate;
                        lastUnsupportedMode.clear();
                        log("Audio input meter attached to device '" + std::string(audioDevice.empty() ? "default" : audioDevice) +
                            "' @ " + std::to_string(sampleRate) + " Hz");
                    }
                }

                int left = -60;
                int right = -60;
                bool hadData = false;
                if (waveMeter.open) {
                    pollWaveLevels(waveMeter, left, right, hadData);

                    if (!hadData && !wasapiMeter.open) {
                        std::string wasapiErr;
                        if (openWasapiInput(wasapiMeter, audioDevice, wasapiErr)) {
                            log("WASAPI fallback capture enabled on endpoint: " +
                                std::string(wasapiMeter.endpointName.empty() ? "(unknown)" : wasapiMeter.endpointName));
                        } else {
                            log("WASAPI fallback open failed: " + wasapiErr);
                        }
                    }
                } else if (!wasapiMeter.open && (validDevId || audioDevice.empty())) {
                    // waveIn failed to open — try WASAPI directly without waiting for !hadData
                    std::string wasapiErr;
                    if (openWasapiInput(wasapiMeter, audioDevice, wasapiErr)) {
                        log("WASAPI direct capture enabled on endpoint: " +
                            std::string(wasapiMeter.endpointName.empty() ? "(unknown)" : wasapiMeter.endpointName));
                    } else {
                        if (lastUnsupportedMode != mode) {
                            log("WASAPI direct open failed: " + wasapiErr);
                            lastUnsupportedMode = mode;
                        }
                    }
                }

                if (wasapiMeter.open) {
                    int wl = -60;
                    int wr = -60;
                    bool wasapiData = false;
                    pollWasapiLevels(wasapiMeter, wl, wr, wasapiData);
                    if (wasapiData) {
                        left = wl;
                        right = wr;
                        hadData = true;
                    }
                }

                if (hadData) {
                    left = calibrateMeterDb(left, gainDb);
                    right = calibrateMeterDb(right, gainDb);
                    lastDataAt = std::chrono::steady_clock::now();
                    lastMeterUpdateAt = lastDataAt;
                    lastMeterL = left;
                    lastMeterR = right;
                } else {
                    auto now = std::chrono::steady_clock::now();
                    if ((now - lastMeterUpdateAt) <= std::chrono::milliseconds(800)) {
                        left = lastMeterL;
                        right = lastMeterR;
                    }
                    if ((now - lastDataAt) > std::chrono::seconds(3) && (now - lastNoDataLogAt) > std::chrono::seconds(5)) {
                        log("No PCM frames received from selected audio device");
                        lastNoDataLogAt = now;
                    }
                }

                {
                    std::lock_guard<std::mutex> lk(m_rtMutex);
                    simplejson::Object rt = readRuntimeState(m_cfgDir);
                    rt.setInt("inputLevelL", left);
                    rt.setInt("inputLevelR", right);
                    stampHealth(rt);
                    writeRuntimeState(m_cfgDir, rt);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                continue;
            }
#else
            if (canSampleAudio) {
                std::string mode = std::string("audio:") + (audioDevice.empty() ? "default" : audioDevice);
                if (lastUnsupportedMode != mode) {
                    log("Audio-device input selected, but direct audio capture telemetry is currently implemented on Windows only.");
                    lastUnsupportedMode = mode;
                }
            }
#endif

            {
                std::lock_guard<std::mutex> lk(m_rtMutex);
                simplejson::Object rt = readRuntimeState(m_cfgDir);
                rt.setInt("inputLevelL", -60);
                rt.setInt("inputLevelR", -60);
                stampHealth(rt);
                writeRuntimeState(m_cfgDir, rt);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        if (levelSocket < 0 || activeAddr != rtpAddr || activePort != rtpPort || activeIface != iface || !connected) {
            if (levelSocket >= 0) close_socket(levelSocket);
            std::string openErr;
            levelSocket = openRtpInputSocket(rtpAddr, rtpPort, iface, openErr);
            activeAddr = rtpAddr;
            activePort = rtpPort;
            activeIface = iface;
            if (levelSocket < 0) {
                {
                    std::lock_guard<std::mutex> lk(m_rtMutex);
                    simplejson::Object rt = readRuntimeState(m_cfgDir);
                    rt.setInt("inputLevelL", -60);
                    rt.setInt("inputLevelR", -60);
                    stampHealth(rt);
                    writeRuntimeState(m_cfgDir, rt);
                }
                if (lastFailure != openErr) {
                    log("Input connect failed: " + openErr);
                    lastFailure = openErr;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            lastFailure.clear();
            lastDataAt = std::chrono::steady_clock::now();
            log("Input meter attached to " + inType + " source " + rtpAddr + ":" + std::to_string(rtpPort)
                + (iface.empty() ? "" : (" via " + iface)));
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(levelSocket, &fds);
        timeval tv{0, 200000};
        int ready = select(levelSocket + 1, &fds, nullptr, nullptr, &tv);

        int left = -60;
        int right = -60;
        if (ready > 0 && FD_ISSET(levelSocket, &fds)) {
            char pkt[2048];
            ssize_t n = recv(levelSocket, pkt, sizeof(pkt), 0);
            if (n > 0) {
                int rawL = -60;
                int rawR = -60;
                if (parseRtpStereoLevels(pkt, static_cast<size_t>(n), rawL, rawR, bitDepth)) {
                    left = calibrateMeterDb(rawL, gainDb);
                    right = calibrateMeterDb(rawR, gainDb);
                    lastDataAt = std::chrono::steady_clock::now();
                    lastMeterUpdateAt = lastDataAt;
                    lastMeterL = left;
                    lastMeterR = right;
                }
            }
        } else {
            auto now = std::chrono::steady_clock::now();
            if ((now - lastMeterUpdateAt) <= std::chrono::milliseconds(800)) {
                left = lastMeterL;
                right = lastMeterR;
            }
            if (connected && (now - lastDataAt) > std::chrono::seconds(3) && (now - lastNoDataLogAt) > std::chrono::seconds(5)) {
                log("No RTP packets received from " + activeAddr + ":" + std::to_string(activePort));
                lastNoDataLogAt = now;
            }
        }

        {
            std::lock_guard<std::mutex> lk(m_rtMutex);
            simplejson::Object rt = readRuntimeState(m_cfgDir);
            rt.setInt("inputLevelL", left);
            rt.setInt("inputLevelR", right);
            stampHealth(rt);
            writeRuntimeState(m_cfgDir, rt);
        }
    }

    if (levelSocket >= 0) close_socket(levelSocket);
#ifdef _WIN32
    closeWaveInput(waveMeter);
    closeWasapiInput(wasapiMeter);
    if (SUCCEEDED(coHr)) CoUninitialize();
#endif
}

// ---------- metadata port listener ----------

void Worker::listenMetaPort() {
    // Default metadata listen port: 9000 + (encoderIdx - 1) * 10
    int metaPort = 9000 + (m_idx - 1) * 10;
    {
        std::ifstream cf(m_cfgDir + "/metadata.json");
        if (cf.is_open()) {
            std::string s((std::istreambuf_iterator<char>(cf)), {});
            simplejson::Object o;
            if (o.parse(s)) {
                metaPort = o.getInt("listenPort", metaPort);
            }
        }
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { log("meta socket() failed"); return; }
    int opt = 1;
#ifdef _WIN32
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(metaPort));
    if (bind(sfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        log("meta bind() failed on port " + std::to_string(metaPort));
        close_socket(sfd);
        return;
    }
    listen(sfd, 8);
    log("Metadata listener on port " + std::to_string(metaPort));

    while (m_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
        timeval tv{1, 0};
        if (select(sfd+1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        sockaddr_in cli{}; socklen_t len = sizeof(cli);
        int cfd = accept(sfd, (sockaddr*)&cli, &len);
        if (cfd < 0) continue;
        char peerIp[INET_ADDRSTRLEN] = {0};
        const char* ipTxt = inet_ntop(AF_INET, &cli.sin_addr, peerIp, sizeof(peerIp));
        std::string peer = std::string(ipTxt ? ipTxt : "unknown") + ":" + std::to_string(ntohs(cli.sin_port));
        log("Metadata client connected: " + peer);
        logSys("Metadata client connected: " + peer);
        // Handle each connection on its own detached thread so accept() stays
        // responsive while long-lived or slow senders hold the socket open.
        std::thread([this, cfd, peer]() {
            auto emitMetadata = [this](const std::string& xmlData) {
                if (xmlData.empty()) return;
                log("Metadata received (" + std::to_string(xmlData.size()) + " bytes)");
                log("META_RAW: " + singleLine(xmlData));
                logSys("META_RAW: " + singleLine(xmlData));

                // Update metadata telemetry in a dedicated file to avoid races
                // with input-level runtime_state writes.
                std::string metaRtPath = m_cfgDir + "/metadata_runtime.json";
                simplejson::Object metaRt = readJsonFile(metaRtPath);
                int count = metaRt.getInt("eventCount", 0);
                metaRt.setInt("eventCount", count + 1);
                metaRt.setString("lastPayloadUtc", timestamp());

                // Store a compact snippet of the raw XML for the UI metadata viewer.
                std::string xmlSnippet = singleLine(xmlData);
                if (xmlSnippet.size() > 500) xmlSnippet = xmlSnippet.substr(0, 500) + "...";
                metaRt.setString("lastRawXml", xmlSnippet);

                // Write to metadata cache file for UI to read
                std::string cachePath = m_cfgDir + "/meta_current.xml";
                std::ofstream mf(cachePath);
                mf << xmlData;

                // Load per-stream configs so the parser settings are applied.
                simplejson::Object aacCfg = readJsonFile(m_cfgDir + "/aac.json");
                simplejson::Object mp3Cfg = readJsonFile(m_cfgDir + "/mp3.json");
                simplejson::Object hlsCfg = readJsonFile(m_cfgDir + "/hls.json");
                simplejson::Object srtCfg = readJsonFile(m_cfgDir + "/srt.json");

                std::string aacFormatted = formattedMetadataForStream("AAC", xmlData, aacCfg);
                std::string mp3Formatted = formattedMetadataForStream("MP3", xmlData, mp3Cfg);
                std::string hlsFormatted = formattedMetadataForStream("HLS", xmlData, hlsCfg);
                std::string srtFormatted = formattedMetadataForStream("SRT", xmlData, srtCfg);

                // Store the formatted HLS payload so the HTTP server can inject it at serve time.
                {
                    std::lock_guard<std::mutex> lk(m_hlsMetaMutex);
                    m_hlsLastMetaPayload = hlsFormatted;
                }

                // Inject metadata into the active HLS playlist EXTINF tags when HLS is running.
                if (m_hlsRunning) {
                    std::string injectErr;
                    bool injected = injectHlsMetadataIntoPlaylist(m_cfgDir, hlsCfg, hlsFormatted, injectErr);
                    if (injected) {
                        simplejson::Object parser = hlsCfg.getSubObject("metaParser");
                        std::string scope = parser.getString("scope", "current");
                        log("HLS playlist metadata injected (scope=" + scope + ")");
                    } else if (injectErr != "playlist not found" && injectErr != "playlist empty") {
                        log("HLS playlist metadata injection skipped: " + injectErr);
                    }
                }

                // Persist per-stream formatted strings for the UI metadata viewer.
                metaRt.setString("lastFormattedAAC", aacFormatted);
                metaRt.setString("lastFormattedMP3", mp3Formatted);
                metaRt.setString("lastFormattedHLS", hlsFormatted);
                metaRt.setString("lastFormattedSRT", srtFormatted);
                std::ofstream metaRtFile(metaRtPath);
                metaRtFile << metaRt.serialize();

                // Push stream-title updates to Icecast servers (out-of-band admin API).
                if (m_aacRunning && aacCfg.getBool("metaEnabled", true)) {
                    std::string u = aacCfg.getString("url",  "");
                    std::string user = aacCfg.getString("user", "source");
                    std::string pw   = aacCfg.getString("pass", "");
                    if (!u.empty()) {
                        std::string af = aacFormatted;
                        std::thread([u,user,pw,af]() { sendIcecastMetaUpdate(u, user, pw, af); }).detach();
                    }
                }
                if (m_mp3Running && mp3Cfg.getBool("metaEnabled", true)) {
                    std::string u = mp3Cfg.getString("url",  "");
                    std::string user = mp3Cfg.getString("user", "source");
                    std::string pw   = mp3Cfg.getString("pass", "");
                    if (!u.empty()) {
                        std::string mf = mp3Formatted;
                        std::thread([u,user,pw,mf]() { sendIcecastMetaUpdate(u, user, pw, mf); }).detach();
                    }
                }

                auto stateTag = [](bool running) { return running ? "RUNNING" : "STOPPED"; };
                std::string aacLine = "META_SENT[AAC][" + std::string(stateTag(m_aacRunning)) + "]: " +
                    (m_aacRunning ? aacFormatted : "not emitted (stream stopped)");
                std::string mp3Line = "META_SENT[MP3][" + std::string(stateTag(m_mp3Running)) + "]: " +
                    (m_mp3Running ? mp3Formatted : "not emitted (stream stopped)");
                std::string hlsLine = "META_SENT[HLS][" + std::string(stateTag(m_hlsRunning)) + "]: " +
                    (m_hlsRunning ? hlsFormatted : "not emitted (stream stopped)");
                std::string srtLine = "META_SENT[SRT][" + std::string(stateTag(m_srtRunning)) + "]: " +
                    (m_srtRunning ? srtFormatted : "not emitted (stream stopped)");
                log(aacLine); logSys(aacLine);
                log(mp3Line); logSys(mp3Line);
                log(hlsLine); logSys(hlsLine);
                log(srtLine); logSys(srtLine);
            };

            std::string xmlData;
            char chunk[4096];
            bool sawPayload = false;
            auto lastIdleLogAt = std::chrono::steady_clock::now() - std::chrono::seconds(20);
            while (m_running) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(cfd, &rfds);
                timeval tv{1, 0};
                int rc = select(cfd + 1, &rfds, nullptr, nullptr, &tv);
                if (rc > 0) {
                    ssize_t n = recv(cfd, chunk, sizeof(chunk), 0);
                    if (n > 0) {
                        xmlData.append(chunk, n);
                        continue;
                    }
                    // Peer closed or socket error.
                    break;
                }
                if (rc == 0) {
                    // Idle gap on a long-lived connection: treat accumulated payload as one event.
                    if (!xmlData.empty()) {
                        sawPayload = true;
                        emitMetadata(xmlData);
                        xmlData.clear();
                    } else {
                        auto now = std::chrono::steady_clock::now();
                        if ((now - lastIdleLogAt) > std::chrono::seconds(15)) {
                            log("Metadata client connected but no payload yet: " + peer);
                            lastIdleLogAt = now;
                        }
                    }
                    continue;
                }
                break;
            }

            if (!xmlData.empty()) {
                sawPayload = true;
                emitMetadata(xmlData);
            }
            if (!sawPayload) {
                log("Metadata client disconnected without payload: " + peer);
            }
            close_socket(cfd);
        }).detach();
    }
    close_socket(sfd);
}
