# Windows build

The result either way is a single `gwemu.exe` with SDL, Dear ImGui and the
compiler runtime linked in: no DLLs beside it, nothing to install.

## Natively, under MSYS2

Open the **MINGW64** shell — not the MSYS one, which builds against the POSIX
emulation layer rather than for Windows itself.

```sh
pacman -S --needed \
    mingw-w64-x86_64-gcc \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-SDL2

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGWEMU_STATIC=ON
cmake --build build
strip build/gwemu.exe
```

`GWEMU_STATIC` selects the `SDL2::SDL2-static` target and links libgcc,
libstdc++ and libwinpthread in, so the executable does not need the MSYS2
runtime and can be copied to a machine that has never seen it. Leave the option
off while developing and it links `SDL2.dll` instead, which builds quicker.

To check the result carries nothing unexpected:

```sh
objdump -p build/gwemu.exe | grep 'DLL Name'
```

Everything listed should be a Windows system DLL.

## Cross-compiled from Linux

```sh
sudo apt install g++-mingw-w64-x86-64 cmake
src/platform/windows/build.sh
```

SDL2 for mingw is fetched into `~/.cache/gwemu-deps` on the first run. The
script warns if the executable ends up needing anything that is not part of
Windows.

## Notes

- The toolchain file selects the **posix** mingw compilers. The win32 thread
  model has no `std::thread`.
- It also restricts `find_package` and friends to the find root. Anything
  looser and the host `/usr/include` wins: glibc headers collide with mingw's
  `corecrt.h`, and `intmax_t` narrows because host `long` is 64-bit.
- `windres` preprocesses through a shell, so every `-I` it is handed has to
  survive one. A repository path containing `&` does not, so the resource
  compile rule drops include paths; the resource needs none.
- SDL renames `main` to `SDL_main` on Windows and supplies the `WinMain` that
  calls it, which is why `SDL2main` is linked.
- `windows.h` still defines `near` and `far` as macros, so neither can be used
  as an identifier in code that includes it.
