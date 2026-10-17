# EXPERIMENTAL cross-toolchain: Linux -> Windows x86 (i686) with MinGW-w64.
#
# This is NOT the officially supported build. The supported configuration is
#   Visual Studio 2022, -A Win32, Debug  (see CMakeLists.txt / VERIFY.md).
#
# Purpose: produce a PE32/i386 OpenSpore.exe from Linux for a Proton runtime probe.
# Compilation success here says nothing about runtime parity; report them separately.
#
# Usage (from the repository root):
#   cmake -S . -B build-mingw32 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=build-tools/mingw-w64-i686.cmake \
#         -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-mingw32 --target OpenSpore
#
# Verified against: i686-w64-mingw32-g++ (GCC) 16.2.0, binutils 2.47, CMake 4.4.4.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  i686-w64-mingw32-windres)

# The i686-w64-mingw32 compiler already defaults to the sysroot below; naming it
# explicitly keeps the cross-root rules coherent for any find_*() the tree adds later.
set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
