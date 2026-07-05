// test_hls.cpp — Unit tests for HLS playlist generation & segment purge policy

#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>

// ---- Minimal inline HLS helpers (real impl lives in worker HLS module) ----

struct HlsSegment {
    std::string filename;
    double durationSec;
    int sequence;
};

/// Generate an HLS playlist string
static std::string generatePlaylist(const std::vector<HlsSegment>& segments,
                                     int mediaSequence, double targetDuration) {
    std::ostringstream ss;
    ss << "#EXTM3U\n"
       << "#EXT-X-VERSION:3\n"
       << "#EXT-X-TARGETDURATION:" << (int)targetDuration << "\n"
       << "#EXT-X-MEDIA-SEQUENCE:" << mediaSequence << "\n";
    for (auto& seg : segments) {
        ss << "#EXTINF:" << seg.durationSec << ",\n" << seg.filename << "\n";
    }
    return ss.str();
}

/// Apply purge policy: keep at most 3x `windowSize` segments; return the ones to delete.
static std::vector<HlsSegment> purgeOld(std::vector<HlsSegment>& segments, int windowSize) {
    std::vector<HlsSegment> removed;
    int retain = std::max(1, windowSize * 3);
    while ((int)segments.size() > retain) {
        removed.push_back(segments.front());
        segments.erase(segments.begin());
    }
    return removed;
}

// ---- Tests ----

TEST_CASE("HLS playlist generation — basic structure", "[hls]") {
    std::vector<HlsSegment> segs = {
        {"seg000.aac", 6.0, 0},
        {"seg001.aac", 6.0, 1},
        {"seg002.aac", 6.0, 2},
    };
    std::string pl = generatePlaylist(segs, 0, 6.0);
    CHECK(pl.find("#EXTM3U") != std::string::npos);
    CHECK(pl.find("#EXT-X-VERSION:3") != std::string::npos);
    CHECK(pl.find("#EXT-X-TARGETDURATION:6") != std::string::npos);
    CHECK(pl.find("#EXT-X-MEDIA-SEQUENCE:0") != std::string::npos);
    CHECK(pl.find("seg000.aac") != std::string::npos);
    CHECK(pl.find("seg002.aac") != std::string::npos);
}

TEST_CASE("HLS purge policy — window 5, 3x back", "[hls]") {
    std::vector<HlsSegment> segs;
    for (int i = 0; i < 18; ++i)
        segs.push_back({"seg" + std::to_string(i) + ".aac", 6.0, i});

    // Window of 5 means we keep 15 segments (3x buffer)
    auto removed = purgeOld(segs, 5);
    CHECK(segs.size() == 15);
    CHECK(removed.size() == 3);
    CHECK(removed[0].filename == "seg0.aac");
    CHECK(removed[1].filename == "seg1.aac");
    CHECK(removed[2].filename == "seg2.aac");
    CHECK(segs.front().filename == "seg3.aac");
}

TEST_CASE("HLS purge policy — no overflow", "[hls]") {
    std::vector<HlsSegment> segs = {{"s0.aac", 6.0, 0}, {"s1.aac", 6.0, 1}};
    auto removed = purgeOld(segs, 5);
    CHECK(removed.empty());
    CHECK(segs.size() == 2);
}

TEST_CASE("HLS media sequence advances correctly", "[hls]") {
    int seq = 10;
    std::vector<HlsSegment> segs;
    for (int i = 0; i < 3; ++i)
        segs.push_back({"seg" + std::to_string(seq + i) + ".aac", 6.0, seq + i});

    std::string pl = generatePlaylist(segs, seq, 6.0);
    CHECK(pl.find("#EXT-X-MEDIA-SEQUENCE:10") != std::string::npos);
}
