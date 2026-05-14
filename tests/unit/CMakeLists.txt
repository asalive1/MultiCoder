cmake_minimum_required(VERSION 3.20)

# Download Catch2 v3 single header if not present
include(FetchContent)
FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(catch2)

# ---- LivewireMapping unit tests ----
add_executable(test_livewire
    test_livewire.cpp
)
target_link_libraries(test_livewire PRIVATE
    livewire_mapping
    Catch2::Catch2WithMain
)
target_include_directories(test_livewire PRIVATE ${CMAKE_SOURCE_DIR}/src/include)

add_test(NAME LivewireMapping COMMAND test_livewire)

# ---- HLS playlist unit tests ----
add_executable(test_hls
    test_hls.cpp
)
target_link_libraries(test_hls PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_hls PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
add_test(NAME HLS COMMAND test_hls)

# ---- Config validation tests ----
add_executable(test_config
    test_config.cpp
)
target_link_libraries(test_config PRIVATE Catch2::Catch2WithMain)
add_test(NAME ConfigValidation COMMAND test_config)

# ---- Metadata parsing tests ----
add_executable(test_metadata
    test_metadata.cpp
)
target_link_libraries(test_metadata PRIVATE Catch2::Catch2WithMain)
add_test(NAME MetadataParsing COMMAND test_metadata)

# ---- Log policy tests ----
add_executable(test_logpolicy
    test_logpolicy.cpp
)
target_link_libraries(test_logpolicy PRIVATE Catch2::Catch2WithMain)
add_test(NAME LogPolicy COMMAND test_logpolicy)

# ---- Simple JSON utility tests ----
add_executable(test_simplejson
    test_simplejson.cpp
    ${CMAKE_SOURCE_DIR}/src/common/SimpleJson.cpp
)
target_include_directories(test_simplejson PRIVATE ${CMAKE_SOURCE_DIR}/src/include)
target_link_libraries(test_simplejson PRIVATE Catch2::Catch2WithMain)
add_test(NAME SimpleJson COMMAND test_simplejson)
