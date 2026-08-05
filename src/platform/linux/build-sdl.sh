#!/bin/sh
# Builds a static SDL2 that opens X11, Wayland and the audio servers at
# runtime rather than linking them.
#
# The distribution's libSDL2.a expects all of those on the link line, which
# would pin the binary to the exact versions on the build machine. Built this
# way, the only hard dependency left is the C library.
set -e

DEPS=${GWEMU_DEPS:-$HOME/.cache/gwemu-deps}
SDL_VERSION=${SDL_VERSION:-2.32.10}
PREFIX="$DEPS/sdl2-linux"

if [ -f "$PREFIX/lib/libSDL2.a" ]; then
    echo "static SDL $SDL_VERSION already built in $PREFIX"
    exit 0
fi

mkdir -p "$DEPS/src"
SRC="$DEPS/src/SDL2-$SDL_VERSION"

if [ ! -d "$SRC" ]; then
    echo "fetching SDL $SDL_VERSION"
    curl -sL -o "$DEPS/src/sdl.tar.gz" \
      "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
    tar xzf "$DEPS/src/sdl.tar.gz" -C "$DEPS/src"
fi

cmake -S "$SRC" -B "$DEPS/build-sdl2" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DSDL_SHARED=OFF -DSDL_STATIC=ON \
      -DSDL_X11_SHARED=ON -DSDL_WAYLAND_SHARED=ON \
      -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO_SHARED=ON \
      -DSDL_TEST=OFF
cmake --build "$DEPS/build-sdl2" --parallel "$(nproc)"
cmake --install "$DEPS/build-sdl2"

echo "static SDL installed in $PREFIX"
