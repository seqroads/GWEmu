# Linux build

```sh
sudo apt install build-essential cmake libsdl2-dev
make
```

That links against the system SDL, which is what you want while developing.

## Release binary

```sh
make dist-linux
```

Produces `dist/gwemu-linux-x86_64`: one static file, around 3.7 MB, which needs
nothing but the C library.

`src/platform/linux/build-sdl.sh` builds the SDL it links against, once, into
`~/.cache/gwemu-deps`. The distribution's `libSDL2.a` is not usable here — its
link line demands X11, Wayland, ALSA, PulseAudio, libdrm and more be present at
link time, which would pin the binary to the exact versions on the build
machine. Built with the `*_SHARED` options instead, SDL opens all of those at
runtime through `dlopen`, so the binary carries none of them and still works on
a desktop running either X11 or Wayland, with whichever audio server.

Check what is left:

```sh
ldd dist/gwemu-linux-x86_64
```

Only `libc`, `libm` and the loader should appear.
