if (WIN32)
    add_library(multicoder_threads INTERFACE)
else()
    find_package(Threads REQUIRED)
    add_library(multicoder_threads INTERFACE)
    target_link_libraries(multicoder_threads INTERFACE Threads::Threads)
endif()

# ---- Supervisor (UI + REST API server) ----
add_executable(multicoder-supervisor
    supervisor/main.cpp
    supervisor/supervisor_api.cpp
    common/SimpleJson.cpp
)
target_include_directories(multicoder-supervisor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/supervisor
)
target_compile_definitions(multicoder-supervisor PRIVATE
    MULTICODER_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(multicoder-supervisor PRIVATE multicoder_threads)
if (WIN32)
    target_link_libraries(multicoder-supervisor PRIVATE ws2_32 iphlpapi winmm)
endif()

# ---- Worker (per-encoder encoding engine) ----
add_executable(multicoder-worker
    worker/main.cpp
    worker/worker.cpp
    livewire/LivewireMapping.cpp
    common/SimpleJson.cpp
)
target_include_directories(multicoder-worker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/worker
)
target_link_libraries(multicoder-worker PRIVATE multicoder_threads)
if (WIN32)
    target_link_libraries(multicoder-worker PRIVATE ws2_32 iphlpapi winmm)
endif()

install(TARGETS multicoder-supervisor multicoder-worker DESTINATION bin)

# On Windows: ensure ffmpeg.exe is present alongside the worker binary so
# the application is fully self-contained (no separate FFmpeg install needed).
if (WIN32)
    add_custom_command(TARGET multicoder-worker POST_BUILD
        COMMAND powershell -ExecutionPolicy Bypass -NonInteractive -File
                "${CMAKE_SOURCE_DIR}/scripts/Get-FFmpeg.ps1"
                -OutDir "$<TARGET_FILE_DIR:multicoder-worker>"
        COMMENT "Ensuring ffmpeg.exe is in the build output directory"
        VERBATIM
    )
endif()

# Export LivewireMapping as a library so tests can link it
add_library(livewire_mapping STATIC livewire/LivewireMapping.cpp)
target_include_directories(livewire_mapping PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
