cmake_minimum_required(VERSION 3.20)

enable_testing()

# Bring in a lightweight test framework (catch2 header-only single header)
# We vendor a minimal shim instead of fetching to keep the build offline-capable.
# Full Catch2 can be added via FetchContent when a network build is desired.

add_subdirectory(unit)
