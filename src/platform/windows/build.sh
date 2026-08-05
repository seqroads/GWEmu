#!/bin/sh
# Cross-builds gwemu.exe for 64-bit Windows: one static executable, no DLLs.
#
# Needs SDL2 for mingw-w64, which is a prebuilt download rather than a build.
set -e

REPO=$(cd "$(dirname "$0")/../../.." && pwd)
DEPS=${GWEMU_DEPS:-$HOME/.cache/gwemu-deps}
SDL_VERSION=${SDL_VERSION:-2.32.10}
SDL="$DEPS/SDL2-$SDL_VERSION/x86_64-w64-mingw32"

if [ ! -d "$SDL" ]; then
    echo "fetching SDL $SDL_VERSION for mingw"
    mkdir -p "$DEPS"
    curl -sL -o "$DEPS/sdl2-mingw.tar.gz" \
      "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
    tar xzf "$DEPS/sdl2-mingw.tar.gz" -C "$DEPS"
fi

BUILD=${1:-$REPO/build-win}

cmake -S "$REPO" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DGWEMU_STATIC=ON \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/src/platform/windows/toolchain-mingw64.cmake" \
      -DCMAKE_FIND_ROOT_PATH="$SDL"

cmake --build "$BUILD" --parallel "$(nproc)"
x86_64-w64-mingw32-strip "$BUILD/gwemu.exe"

echo
echo "built $BUILD/gwemu.exe"
ls -la "$BUILD/gwemu.exe"

# Nothing outside the Windows system set may be needed at runtime. A DLL that
# also exists in the toolchain is one we were supposed to have linked in.
bad=0
for dep in $(x86_64-w64-mingw32-objdump -p "$BUILD/gwemu.exe" | awk '/DLL Name:/ {print $3}'); do
    if find "$SDL" /usr/x86_64-w64-mingw32 /usr/lib/gcc/x86_64-w64-mingw32 \
            -name "$dep" 2>/dev/null | grep -q .; then
        echo "error: needs $dep, which is not part of Windows" >&2
        bad=1
    fi
done
exit "$bad"
