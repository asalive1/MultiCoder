#include <catch2/catch_test_macros.hpp>
#include <vector>

static int rotateCountForSize(long bytes, long rotateBytes) {
    if (rotateBytes <= 0) return 0;
    return static_cast<int>(bytes / rotateBytes);
}

static std::vector<int> retainLastDays(const std::vector<int>& ageDays, int keepDays) {
    std::vector<int> kept;
    for (int age : ageDays) {
        if (age <= keepDays) kept.push_back(age);
    }
    return kept;
}

TEST_CASE("log rotation threshold default 10MB", "[log]") {
    const long tenMB = 10L * 1024L * 1024L;
    CHECK(rotateCountForSize(tenMB - 1, tenMB) == 0);
    CHECK(rotateCountForSize(tenMB, tenMB) == 1);
    CHECK(rotateCountForSize(25L * 1024L * 1024L, tenMB) == 2);
}

TEST_CASE("log retention default 14 days", "[log]") {
    std::vector<int> ages = {0, 1, 7, 14, 15, 30};
    auto kept = retainLastDays(ages, 14);
    REQUIRE(kept.size() == 4);
    CHECK(kept[0] == 0);
    CHECK(kept[1] == 1);
    CHECK(kept[2] == 7);
    CHECK(kept[3] == 14);
}
