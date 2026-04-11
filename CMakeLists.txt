# =============================================================================
# Last modified: 2026-04-11 11:53
# CMakeLists.txt — Build system for the Facón chess engine
#
# Changelog:
#   1.2 — Replace global -static flag with selective static linking on Windows.
#         Added src/version.rc: Windows version resource.
#
#   1.3 — Centralized version management via configure_file. A single source
#         of truth: PROJECT_VERSION and FACON_CODENAME in this file control
#         the binary name, the startup banner, the UCI id name, and the
#         Windows version resource. To change the version, edit only the
#         two lines marked below.
#         Build directories renamed: build -> build-linux, for consistency
#         with build-windows.
#
# QUICK START
# -----------
# Linux (portable binary):
#   mkdir build-linux && cd build-linux
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
#
# Linux (optimized for YOUR machine, not distributable):
#   mkdir build-linux && cd build-linux
#   cmake .. -DCMAKE_BUILD_TYPE=Release -DNATIVE=ON
#   make -j$(nproc)
#
# Windows native (MSVC or MinGW):
#   mkdir build-windows && cd build-windows
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   cmake --build . --config Release
#
# Windows cross-compile from Linux (MinGW-w64):
#   mkdir build-windows && cd build-windows
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/windows-cross.cmake \
#            -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
# =============================================================================

cmake_minimum_required(VERSION 3.16)

# =============================================================================
# VERSION — CHANGE THESE TWO LINES FOR EACH RELEASE
# =============================================================================
# During development: FACON_CODENAME = "dev"
#   binary name = facon-1.3-dev,  UCI id = "Facon 1.3-dev"
# For release:        FACON_CODENAME = "Yunque"
#   binary name = facon-1.3,      UCI id = "Facon 1.3 - Yunque"

project(facon VERSION 1.3 LANGUAGES CXX)
set(FACON_CODENAME "Yunque")

# =============================================================================
# VERSION STRING CONSTRUCTION
# =============================================================================

if(FACON_CODENAME STREQUAL "dev")
    set(FACON_VERSION_STRING "${PROJECT_VERSION}-dev")
    set(FACON_OUTPUT_NAME    "facon-${PROJECT_VERSION}-dev")
else()
    set(FACON_VERSION_STRING "${PROJECT_VERSION} - ${FACON_CODENAME}")
    set(FACON_OUTPUT_NAME    "facon-${PROJECT_VERSION}")
endif()

message(STATUS "Version string : ${FACON_VERSION_STRING}")
message(STATUS "Output name    : ${FACON_OUTPUT_NAME}")

# Generate version.h from version.h.in — included by main.cpp and uci.cpp.
# Never edit the generated version.h directly; edit CMakeLists.txt instead.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

# =============================================================================
# OPTIONS
# =============================================================================

option(NATIVE "Optimize for the current CPU (-march=native). Not distributable." OFF)
option(LTO    "Enable Link Time Optimization. May slow down compilation."        ON)

# =============================================================================
# C++ STANDARD
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# DEFAULT BUILD TYPE
# =============================================================================

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
    message(STATUS "No build type specified — defaulting to Release")
endif()

# =============================================================================
# SOURCE FILES
# =============================================================================

set(SOURCES
    src/main.cpp
    src/bitboard.cpp
    src/board.cpp
    src/movegen.cpp
    src/eval.cpp
    src/tt.cpp
    src/timeman.cpp
    src/search.cpp
    src/uci.cpp
)

add_executable(facon ${SOURCES})
set_target_properties(facon PROPERTIES OUTPUT_NAME "${FACON_OUTPUT_NAME}")

# Make the generated version.h visible to all source files.
target_include_directories(facon PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# =============================================================================
# COMPILER FLAGS
# =============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")

    # --- Warnings ---
    target_compile_options(facon PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    # --- Release flags ---
    target_compile_options(facon PRIVATE
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Release>:-DNDEBUG>
        $<$<CONFIG:Release>:-fno-exceptions>   # We don't use exceptions
        $<$<CONFIG:Release>:-funroll-loops>    # Helps with bitboard iteration loops
    )

    # --- Debug flags ---
    target_compile_options(facon PRIVATE
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
    target_link_options(facon PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )

    # --- CPU target ---
    # POPCNT is a hard requirement: we use popcount() in every bitboard loop.
    # Without it the compiler emulates it in software — significant slowdown.
    # -mpopcnt is supported by all x86-64 CPUs made after ~2008.
    if(NATIVE)
        message(STATUS "CPU target: native (not distributable)")
        target_compile_options(facon PRIVATE -march=native)
    else()
        message(STATUS "CPU target: x86-64 + POPCNT (distributable)")
        target_compile_options(facon PRIVATE -mpopcnt)
    endif()

    # --- Link Time Optimization ---
    if(LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if(ipo_supported)
            set_property(TARGET facon PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "LTO: enabled")
        else()
            message(STATUS "LTO: not supported by this compiler (${ipo_error})")
        endif()
    endif()

elseif(MSVC)

    # --- MSVC flags ---
    target_compile_options(facon PRIVATE
        $<$<CONFIG:Release>:/O2>       # Maximum optimization
        $<$<CONFIG:Release>:/Ob2>      # Aggressive inlining
        $<$<CONFIG:Release>:/DNDEBUG>
        $<$<CONFIG:Release>:/GL>       # Whole-program optimization (MSVC LTO)
        $<$<CONFIG:Debug>:/Od>         # No optimization
        $<$<CONFIG:Debug>:/Zi>         # Debug info
        /W4                            # Warning level 4
        /wd4996                        # Suppress deprecated CRT warnings
        /arch:SSE4.2                   # Enables POPCNT on MSVC
    )

    if(NATIVE)
        message(STATUS "CPU target: native")
        target_compile_options(facon PRIVATE /arch:AVX2)
    endif()

endif()

# =============================================================================
# WINDOWS-SPECIFIC SETTINGS
# =============================================================================

if(WIN32)
    target_compile_definitions(facon PRIVATE
        _CRT_SECURE_NO_WARNINGS   # Suppress MSVC secure function warnings
        WIN32_LEAN_AND_MEAN       # Exclude rarely-used Windows headers
        NOMINMAX                  # Prevent Windows.h from defining min/max macros
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(facon PRIVATE
            -static-libgcc
            -static-libstdc++
            -Wl,-Bstatic,-lwinpthread,-Bdynamic
        )
    endif()

    # Generate version.rc from version.rc.in and add to the build.
    # The generated file contains the correct version numbers from PROJECT_VERSION.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        @ONLY
    )
    target_sources(facon PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
endif()

# =============================================================================
# BUILD SUMMARY
# =============================================================================

message(STATUS "-------------------------------------------")
message(STATUS "Facon ${FACON_VERSION_STRING}")
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Platform   : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Native CPU : ${NATIVE}")
message(STATUS "LTO        : ${LTO}")
message(STATUS "-------------------------------------------")
