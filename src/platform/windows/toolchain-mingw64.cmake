# Cross-compilation to 64-bit Windows with mingw-w64.
#
#   cmake -S . -B build-win \
#         -DCMAKE_TOOLCHAIN_FILE=src/platform/windows/toolchain-mingw64.cmake \
#         -DCMAKE_PREFIX_PATH="<qt6-mingw-prefix>;<sdl3-mingw-prefix>"

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# The -posix variants are required: the win32 thread model has no std::thread,
# which Qt and the frontend both rely on.
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Extra dependency prefixes (Qt, SDL) are appended here by the caller rather
# than through CMAKE_PREFIX_PATH, because only paths under the find root are
# searched once the modes below are set.
list(APPEND CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Host programs (moc, cmake) come from the host; libraries, headers and
# packages must come only from the find root. Anything looser lets the host's
# /usr/include win: the Linux glibc headers then collide with mingw's, and a
# Windows target happily "finds" Fontconfig, Wayland and EGL.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
