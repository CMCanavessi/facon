# =============================================================================
# Last modified: 2026-06-06 19:27
# CMakeLists.txt -- Build system for the Facon chess engine
#
# Facon 1.0 -- Oxido
#   - Initial CMake build system. Out-of-source build (build/), Release and
#     Debug configurations, GCC/Clang support, basic flags (-O3/-DNDEBUG
#     release, -O0/-g debug).
#
# Facon 1.2 -- Rojo Vivo
#   - Replaced global -static flag with selective static linking on Windows
#     (only libgcc, libstdc++, winpthread).
#   - Added src/version.rc as a static Windows version resource.
#
# Facon 1.3 -- Yunque
#   - Centralized version management via configure_file. PROJECT_VERSION and
#     FACON_CODENAME in this file are the single source of truth for binary
#     name, startup banner, UCI id name, and Windows version resource. Edit
#     only the two lines marked below to change the version.
#   - Build directories renamed: build -> build-linux for consistency with
#     build-windows.
#   - src/version.rc replaced by src/version.rc.in (template) -- the .rc file
#     is now generated from PROJECT_VERSION at configure time.
#
# Facon 1.4 -- Hoja
#   - Version bumped to 1.4 / "Hoja" for release.
#   - FACON_DEBUG option for diagnostic counters (LMR/NMP attempted/cutoffs,
#     TT cutoffs). Reported via "info string ST:" after each iteration in
#     debug builds. Zero cost in release.
#   - NATIVE option for -march=native (not distributable; opt-in for personal
#     builds).
#   - LTO option (default ON, falls back gracefully if the compiler does not
#     support interprocedural optimization).
#   - dev codename matching with MATCHES "^dev" -- supports any codename
#     starting with "dev" (dev, dev-rc1, dev-NN-*, etc.).
#
# Facon 1.5 -- Espiga
#   - Version bumped to 1.5. Codename "Espiga" for release.
#   - MSVC support removed. The previous MSVC branch was non-functional
#     because bitboard.h uses GCC/Clang builtins (__builtin_popcountll,
#     __builtin_ctzll, __builtin_clzll) directly without wrappers. Keeping
#     dead code gave a false sense of portability. Native Windows builds
#     should use MinGW-w64 (either natively or via cross-compile from
#     Linux). MSVC is now rejected at configure time with FATAL_ERROR.
#   - Added -fomit-frame-pointer to Release builds. Frees %rbp as a
#     general-purpose register in the hot search/eval paths. Measured
#     ~+2.3% NPS over the previous default flag set on Ryzen 7 1700.
#
# Facon 1.6 -- Temple
#   - Version bumped to 1.6. Codename "Temple".
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
# Windows native (MinGW-w64):
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
# VERSION -- CHANGE THESE TWO LINES FOR EACH RELEASE
# =============================================================================
# Set PROJECT_VERSION (in project() below) and FACON_CODENAME to taste.
#
# During development, use a codename starting with "dev". The version string
# and binary name then carry that suffix, keeping dev builds distinguishable:
#     FACON_CODENAME = "dev-..."
#       version string = "<version>-<codename>"   (e.g. used in the UCI id)
#       binary name    = "facon-<version>-<codename>"
#
# For a release, use the plain release codename (anything not starting with
# "dev"). The suffix is dropped from the binary and the version string switches
# to the release format:
#     FACON_CODENAME = "<codename>"
#       version string = "<version> - <codename>"
#       binary name    = "facon-<version>"
#
# The rule below ("^dev") is what selects between the two formats, so no other
# line needs editing when moving between development and release.

project(facon VERSION 1.6 LANGUAGES CXX)
set(FACON_CODENAME "Temple")

# =============================================================================
# VERSION STRING CONSTRUCTION
# =============================================================================

if(FACON_CODENAME MATCHES "^dev")
    set(FACON_VERSION_STRING "${PROJECT_VERSION}-${FACON_CODENAME}")
    set(FACON_OUTPUT_NAME    "facon-${PROJECT_VERSION}-${FACON_CODENAME}")
else()
    set(FACON_VERSION_STRING "${PROJECT_VERSION} - ${FACON_CODENAME}")
    set(FACON_OUTPUT_NAME    "facon-${PROJECT_VERSION}")
endif()

message(STATUS "Version string : ${FACON_VERSION_STRING}")
message(STATUS "Output name    : ${FACON_OUTPUT_NAME}")

# Generate version.h from version.h.in -- included by main.cpp and uci.cpp.
# Never edit the generated version.h directly; edit CMakeLists.txt instead.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

# =============================================================================
# OPTIONS
# =============================================================================

option(NATIVE      "Optimize for the current CPU (-march=native). Not distributable." OFF)
option(LTO         "Enable Link Time Optimization. May slow down compilation."        ON)
option(FACON_DEBUG "Enable diagnostic counters (LMR/NMP stats per iteration)."        OFF)

if(FACON_DEBUG)
    add_compile_definitions(FACON_DEBUG)
    message(STATUS "FACON_DEBUG: enabled (diagnostic counters active)")
endif()

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
    message(STATUS "No build type specified -- defaulting to Release")
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
# Facon supports only GCC and Clang (including MinGW-w64 for Windows builds).
# MSVC is not supported: the source uses GCC/Clang builtins like
# __builtin_popcountll, __builtin_ctzll, and __builtin_clzll directly,
# without MSVC wrappers. Native Windows builds should use MinGW-w64
# (either natively or via cross-compile from Linux).

if(MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR
        "Facon does not support MSVC. Use GCC or Clang on Linux, "
        "or MinGW-w64 on Windows (or cross-compile from Linux with "
        "the toolchain file in cmake/windows-cross.cmake)."
    )
endif()

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
        $<$<CONFIG:Release>:-fno-exceptions>      # We don't use exceptions
        $<$<CONFIG:Release>:-funroll-loops>       # Helps with bitboard iteration loops
        $<$<CONFIG:Release>:-fomit-frame-pointer> # Frees %rbp as a general-purpose
                                                  # register in hot code paths.
                                                  # Measured ~+2.3% NPS at 1.5.
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
    # Without it the compiler emulates it in software -- significant slowdown.
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

endif()

# =============================================================================
# WINDOWS-SPECIFIC SETTINGS
# =============================================================================

if(WIN32)
    target_compile_definitions(facon PRIVATE
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
message(STATUS "Debug stats: ${FACON_DEBUG}")
message(STATUS "-------------------------------------------")
