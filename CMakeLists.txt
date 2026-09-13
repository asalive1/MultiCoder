cmake_minimum_required(VERSION 3.20)
project(MultiCoder VERSION 2.5.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(MULTICODER_ENABLE_SANITIZERS "Enable ASan+UBSan for Debug builds on supported toolchains" OFF)
option(MULTICODER_ENABLE_CLANG_TIDY "Enable clang-tidy during compilation when available" OFF)

if(MULTICODER_ENABLE_SANITIZERS)
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
		message(WARNING "MULTICODER_ENABLE_SANITIZERS is intended for Debug builds")
	endif()

	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
		add_compile_options(
			$<$<CONFIG:Debug>:-fsanitize=address,undefined>
			$<$<CONFIG:Debug>:-fno-omit-frame-pointer>
		)
		add_link_options(
			$<$<CONFIG:Debug>:-fsanitize=address,undefined>
			$<$<CONFIG:Debug>:-fno-omit-frame-pointer>
		)
	else()
		message(WARNING "Sanitizers requested, but compiler '${CMAKE_CXX_COMPILER_ID}' is not supported by this toggle")
	endif()
endif()

if(MULTICODER_ENABLE_CLANG_TIDY)
	find_program(MULTICODER_CLANG_TIDY_EXE NAMES clang-tidy)
	if(MULTICODER_CLANG_TIDY_EXE)
		set(CMAKE_CXX_CLANG_TIDY ${MULTICODER_CLANG_TIDY_EXE})
	else()
		message(WARNING "MULTICODER_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
	endif()
endif()

include(CTest)

add_subdirectory(src)
if(BUILD_TESTING)
	add_subdirectory(tests)
endif()
