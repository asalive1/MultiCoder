#pragma once

// IntelliSense fallback header for environments where Catch2 headers are not yet
// fetched/configured by CMake. Real test builds should prefer Catch2::Catch2WithMain
// include directories from FetchContent.

#ifndef CATCH2_VERSION_MAJOR
#define CATCH2_VERSION_MAJOR 3
#endif

// Minimal macro fallbacks to keep parser happy if real Catch2 is unavailable.
#ifndef TEST_CASE
#define TEST_CASE(name, tags) static void CATCH2_FALLBACK_TEST_##__LINE__()
#endif
#ifndef CHECK
#define CHECK(expr) do { (void)(expr); } while (0)
#endif
#ifndef REQUIRE
#define REQUIRE(expr) do { (void)(expr); } while (0)
#endif
#ifndef CHECK_FALSE
#define CHECK_FALSE(expr) do { (void)(expr); } while (0)
#endif
