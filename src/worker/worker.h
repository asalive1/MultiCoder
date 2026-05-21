#pragma once
#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <fstream>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

struct HlsScteRangeState {
    bool active{false};
    std::string id;
    std::string eventId;
    std::string actionOut;
    std::string actionIn;
    std::string cueOut;
    std::string cueIn;
    std::string startDateUtc;
    std::string endDateUtc;
    int64_t startEpochMs{0};
    int64_t endEpochMs{0};
};

/// Worker manages one encoder instance.
/// It reads per-encoder JSON configs and starts/stops stream sub-processes.
class Worker {
public:
    Worker(int idx, const std::string& cfgDir);
    ~Worker();

    /// Block until SIGTERM/SIGINT
    void run();

    /// Start/stop individual streams (called from control listener)
    void startAAC();
    void stopAAC();
    void startMP3();
    void stopMP3();
    void startHLS();
    void stopHLS();
    void startSRT();
    void stopSRT();

private:
    void log(const std::string& msg);
    void logSys(const std::string& msg);
    void initWindowsFirewallRules();
    void publishStreamHealth();
    void pollSinkProcesses();
    void listenControlPort();
    void listenMetaPort();
    void listenCueTcpPort();
    void monitorInputLevels();
    bool executeControlCommand(const std::string& cmd,
                               const std::string& source,
                               const std::string& eventId,
                               const std::string& cueValue);
    void emitScteSidecarEvent(const std::string& action,
                              const std::string& eventId,
                              const std::string& cueValue,
                              const std::string& source,
                              bool primaryPathLikelyAvailable);

    // Helper: retrieve input gain in dB (combines input.json rtpGain + session override)
    double getInputGainDb();
    // Helper: build FFmpeg audio filter string for gain (empty if gain == 0)
    std::string buildAudioFilterWithGain(double gainDb);

    // Real-time gain: polls sessionGainDb and restarts active streams when gain settles
    void monitorGainChanges();
    std::thread m_gainMonitorThread;
    // Mutex serialising concurrent stream start/stop from control and gain monitor threads
    std::recursive_mutex m_streamMutex;

    // SRT input relay: FFmpeg process that receives an SRT source and re-muxes to
    // raw PCM s16le UDP multicast (239.255.127.1:RELAYPORT) so all encoders and the
    // VU meter share one connection.  Started by StartSRTInput control command.
    void startSRTInputRelay();
    void stopSRTInputRelay();
    void* m_srtInputRelayProc{nullptr};
    std::atomic<bool> m_srtInputRelayRunning{false};

    int m_idx;
    std::string m_cfgDir;
    std::string m_logPath;
    std::ofstream m_logFile;
    std::mutex m_logMutex;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_aacRunning{false};
    std::atomic<bool> m_mp3Running{false};
    std::atomic<bool> m_hlsRunning{false};
    std::atomic<bool> m_srtRunning{false};
    std::atomic<bool> m_controlListenerRunning{false};
    std::atomic<bool> m_cueListenerRunning{false};
    std::mutex m_rtMutex;  // guards all runtime_state.json reads and writes

    std::thread m_controlThread;
    std::thread m_metaThread;
    std::thread m_cueThread;
    std::thread m_inputLevelThread;

    // Dedicated HLS HTTP playback server (serves /hls/* from the encoder's hls dir)
    std::atomic<bool>  m_hlsHttpRunning{false};
    std::atomic<int>   m_hlsHttpSocket{-1};
    std::thread        m_hlsHttpThread;
    void serveHlsHttp(int port, const std::string& hlsDir);

    // HLS segment monitoring — logs each new .ts segment as FFmpeg writes it
    std::atomic<bool>  m_hlsSegMonRunning{false};
    std::thread        m_hlsSegMonThread;
    void monitorHlsSegments(const std::string& hlsDir);

    // Current HLS metadata payload — injected into playlist at serve time.
    std::mutex  m_hlsMetaMutex;
    std::string m_hlsLastMetaPayload;
    std::mutex m_hlsScteMutex;
    HlsScteRangeState m_hlsScteRange;

    // FFmpeg subprocess handles — nullptr means not running.
    // Windows: stored as HANDLE cast to void*.
    // Linux:   stored as (void*)(intptr_t)pid; nullptr == pid 0 == not running.
    void* m_aacProc{nullptr};
    void* m_mp3Proc{nullptr};
    void* m_hlsProc{nullptr};
    void* m_srtProc{nullptr};

    // FFmpeg helpers
    // Returns INVALID_HANDLE_VALUE on failure; logs result to encoder log.
    void   killFfmpegProc(void* &h);
    // Build the input portion of an FFmpeg command from /etc/encoder{N}/input.json.
    // Returns a vector of arg strings that can be prepended to -vn -c:a ...
    std::vector<std::string> buildFfmpegInputArgs();
    // Launch an FFmpeg subprocess with the given full argument list.
    // Returns Windows HANDLE (cast to void*) or nullptr on failure.
    void* launchFfmpeg(const std::vector<std::string>& args, const std::string& tag);
    bool  ffmpegProcAlive(void* h);
};
