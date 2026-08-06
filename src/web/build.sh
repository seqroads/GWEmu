#!/bin/sh
# Builds the browser version into dist/web/.
#
# Needs the Emscripten SDK on PATH, or installed at $EMSDK / ~/emsdk:
#
#   git clone https://github.com/emscripten-core/emsdk ~/emsdk
#   ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
#
# The result is static files: serve dist/web over any HTTP server.
set -e

root=$(cd "$(dirname "$0")/../.." && pwd)
out=$root/dist/web

# emsdk_env.sh insists on being sourced from its own directory, so put the
# tools on PATH directly instead.
if ! command -v emcc >/dev/null 2>&1; then
    for sdk in "$EMSDK" "$HOME/emsdk" /usr/lib/emsdk /usr/share/emscripten; do
        if [ -n "$sdk" ] && [ -x "$sdk/upstream/emscripten/emcc" ]; then
            PATH="$sdk/upstream/emscripten:$sdk/node/$(ls "$sdk/node" 2>/dev/null | head -1)/bin:$PATH"
            export PATH
            break
        fi
    done
fi
command -v emcc >/dev/null 2>&1 || {
    echo "emcc not found. Install the Emscripten SDK; see the header of this script." >&2
    exit 1
}

mkdir -p "$out"
cp "$root"/src/web/shell/* "$out/"

# Nothing else parses the shell before a browser does, and a browser reports a
# broken module as a page that quietly does nothing. Emscripten brings a node
# along, so use it to fail the build instead.
if command -v node >/dev/null 2>&1; then
    for js in "$out"/*.js; do
        case $(basename "$js") in gwcore.js) continue ;; esac
        node --input-type=module --check < "$js" || {
            echo "syntax error in $(basename "$js")" >&2
            exit 1
        }
    done
fi

# The interpreter is the whole cost, so it gets everything the compiler has.
# No filesystem, no exceptions and a fixed heap: each of those is a check on
# every call or access that this program never needs.
emcc -O3 -flto \
    -I"$root/src/core" \
    "$root"/src/core/cpu.c \
    "$root"/src/core/bus.c \
    "$root"/src/core/periph.c \
    "$root"/src/core/octospi.c \
    "$root"/src/core/aes.c \
    "$root"/src/core/cryp.c \
    "$root"/src/core/state.c \
    "$root"/src/core/emu.c \
    "$root"/src/web/web_main.c \
    -o "$out/gwcore.js" \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s EXPORT_NAME=GWCore \
    -s ENVIRONMENT=web,worker \
    -s FILESYSTEM=0 \
    -s INITIAL_MEMORY=64MB \
    -s ALLOW_MEMORY_GROWTH=0 \
    -s STACK_SIZE=1MB \
    -s DISABLE_EXCEPTION_CATCHING=1 \
    -s EXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAP16,HEAPU32 \
    -s EXPORTED_FUNCTIONS=_malloc,_free \
    --no-entry

echo "built $out"
ls -lh "$out"
