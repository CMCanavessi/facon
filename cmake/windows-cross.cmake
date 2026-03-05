# =============================================================================
# cmake/windows-cross.cmake — MinGW-w64 cross-compilation toolchain
#
# Configures CMake to cross-compile for Windows x64 from Linux.
#
# Usage:
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/windows-cross.cmake \
#            -DCMAKE_BUILD_TYPE=Release
#
# Requirements:
#   sudo apt install mingw-w64
# =============================================================================

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# MinGW-w64 compiler toolchain
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Root path for the target environment (headers and libraries)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Never search for programs in the target environment (use host tools)
# Always search for headers and libraries in the target environment
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
