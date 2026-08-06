# GWEmu — one binary per platform, no dependencies.
#
#   make            build ./build/gwemu against the system SDL
#   make run        build and run
#   make web        build the browser version into dist/web/
#   make serve      build it and serve it at http://localhost:8000
#   make dist       static binaries in dist/, for release
#   make clean
#
# `make dist` needs SDL2 built for each target. See src/platform/*/README.md;
# src/platform/windows/build.sh fetches the Windows one for you. `make web`
# needs the Emscripten SDK; src/web/build.sh says where to get it.

BUILD  ?= build
JOBS   ?= $(shell nproc 2>/dev/null || echo 4)
PORT   ?= 8000

.PHONY: all run web serve dist dist-linux dist-windows clean

all:
	@cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release >/dev/null
	@cmake --build $(BUILD) -j$(JOBS)

run: all
	@./$(BUILD)/gwemu

web:
	@src/web/build.sh

# Module workers and WebAssembly need a real origin; file:// will not do.
serve: web
	@echo "http://localhost:$(PORT)/"
	@cd dist/web && python3 -m http.server $(PORT)

dist: dist-linux dist-windows
	@ls -lh dist/

# SDL still opens X11, Wayland and the audio servers at runtime rather than
# linking them, so a static build stays portable across desktops.
dist-linux:
	@mkdir -p dist
	@src/platform/linux/build-sdl.sh
	@cmake -S . -B $(BUILD)-static -DCMAKE_BUILD_TYPE=Release -DGWEMU_STATIC=ON \
	       -DCMAKE_PREFIX_PATH="$(HOME)/.cache/gwemu-deps/sdl2-linux" >/dev/null
	@cmake --build $(BUILD)-static -j$(JOBS)
	@cp $(BUILD)-static/gwemu dist/gwemu-linux-x86_64

dist-windows:
	@mkdir -p dist
	@src/platform/windows/build.sh
	@cp build-win/gwemu.exe dist/gwemu-win64.exe

clean:
	@rm -rf $(BUILD) $(BUILD)-static build-win dist
