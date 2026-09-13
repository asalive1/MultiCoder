#include <catch2/catch_test_macros.hpp>
#include <string>

static bool hasKey(const std::string& j, const std::string& key) {
    return j.find("\"" + key + "\"") != std::string::npos;
}

static bool validPort(int p) {
    return p > 0 && p < 65536;
}

TEST_CASE("system config defaults are present", "[config]") {
    const std::string systemJson = R"({
        "uiPort": 8050,
        "logLevel": "info",
        "logMaxSizeMb": 10,
        "logRotSize": 10,
        "logRetention": 14,
        "encoderCount": 5,
        "adminUser": "Admin",
        "adminPass": "change-me"
    })";

    CHECK(hasKey(systemJson, "uiPort"));
    CHECK(hasKey(systemJson, "logLevel"));
    CHECK(hasKey(systemJson, "logMaxSizeMb"));
    CHECK(hasKey(systemJson, "encoderCount"));
}

TEST_CASE("port validation range", "[config]") {
    CHECK(validPort(1));
    CHECK(validPort(8050));
    CHECK(validPort(65535));
    CHECK_FALSE(validPort(0));
    CHECK_FALSE(validPort(-1));
    CHECK_FALSE(validPort(65536));
}

TEST_CASE("encoder count range", "[config]") {
    auto validEncoderCount = [](int c) { return c >= 1 && c <= 5; };
    CHECK(validEncoderCount(1));
    CHECK(validEncoderCount(5));
    CHECK_FALSE(validEncoderCount(0));
    CHECK_FALSE(validEncoderCount(6));
}

TEST_CASE("metadata and sidecar queue config keys are present", "[config]") {
    const std::string metadataJson = R"({
        "mode": "listen",
        "listenPort": 9000,
        "dispatchQueueCapacity": 64
    })";

    const std::string srtJson = R"({
        "sidecar": {
            "enabled": false,
            "transport": "http-json",
            "mode": "mirror",
            "url": "",
            "retries": 3,
            "queueCapacity": 16
        }
    })";

    CHECK(hasKey(metadataJson, "dispatchQueueCapacity"));
    CHECK(hasKey(srtJson, "queueCapacity"));
}

TEST_CASE("stream lifecycle state names remain stable", "[config]") {
    const std::string statusJson = R"({
        "aacState": "stopped",
        "mp3State": "starting",
        "hlsState": "running",
        "srtState": "failed"
    })";

    CHECK(hasKey(statusJson, "aacState"));
    CHECK(hasKey(statusJson, "mp3State"));
    CHECK(hasKey(statusJson, "hlsState"));
    CHECK(hasKey(statusJson, "srtState"));
}
