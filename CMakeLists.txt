# =============================================================================
# CMakeLists.txt — Build system for the Facón chess engine
#
# QUICK START
# -----------
# Linux (portable binary):
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
#
# Linux (optimized for YOUR machine, not distributable):
#   cmake .. -DCMAKE_BUILD_TYPE=Release -DNATIVE=ON
#   make -j$(nproc)
#
# Windows native (MSVC or MinGW):
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

project(facon VERSION 1.0 LANGUAGES CXX)

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
set_target_properties(facon PROPERTIES OUTPUT_NAME "facon-1.0")

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

    # Static linking: the .exe runs on any Windows machine without extra DLLs
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(facon PRIVATE
            -static
            -static-libgcc
            -static-libstdc++
        )
    endif()
endif()

# =============================================================================
# BUILD SUMMARY
# =============================================================================

message(STATUS "-------------------------------------------")
message(STATUS "Facon ${PROJECT_VERSION}")
message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Platform   : ${CMAKE_SYSTEM_NAME}")
message(STATUS "Native CPU : ${NATIVE}")
message(STATUS "LTO        : ${LTO}")
message(STATUS "-------------------------------------------")
