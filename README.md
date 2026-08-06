# GWEmu

An emulator for the Nintendo Game & Watch (2020) — the Super Mario Bros. and
Zelda handhelds, which are STM32H7B0 microcontrollers under the shell. Both
boot, play, keep time and save.

Nothing is reimplemented at the game level. The unmodified factory ARM firmware
dumped from a real unit runs as-is: it decrypts its own external flash through
an emulated OTFDEC, decrypts its bundled NES and Game Boy ROMs through an
emulated AES engine, drives an emulated LTDC, and paints real frames.

Firmware dumps are **not** distributed here. Use a dump from your own unit —
see [game-and-watch-backup](https://github.com/ghidraninja/game-and-watch-backup).

## Running

```sh
gwemu                              # opens empty; File > Open Firmware
gwemu internal_flash_backup.bin    # or name the dump directly
```

Pick either half of the pair — the other is found beside it by size, since a
128 KiB image is the internal flash and a 1 or 4 MiB image is the external one.

Default keys: **arrows** for the D-pad, **X** and **Z** for A and B, **1** GAME,
**2** TIME, **3** PAUSE/SET, **4** SELECT and **5** START on Zelda. POWER is on
the Emulator menu rather than a key, so it cannot be hit by accident. Gamepads
work, including the sticks and triggers; rebind anything under Config ▸ Input.

`F5` and `F8` save and load a state, `F12` takes a screenshot, `F11` is
fullscreen. Save data, save states and screenshots live in the platform's
user-data directory, keyed by a hash of the firmware so two dumps never collide.

## In a browser

**[Play it here](https://seqroads.github.io/GWEmu/)** — bring your own dump.

The same core compiles to WebAssembly and runs in a tab, with no install and
nothing uploaded: the dump is read locally, kept in the browser's own storage,
and never sent anywhere. It reaches full speed on ordinary hardware — the
interpreter measures 1.8x real time in WebAssembly against 2.5x native on the
same machine.

```sh
make serve        # builds dist/web and serves it at http://localhost:8000
make web          # just build, into dist/web/
```

`dist/web` is static files and can be published anywhere; it needs no headers
and no server logic. Building it needs the Emscripten SDK, which
[src/web/build.sh](src/web/build.sh) explains how to install and finds by
itself once it is there.

Drop both flash images onto the page or pick them with the button. The unit
powers itself on, as it does natively, and keyboard and gamepad use the same
controls as the desktop build. Save data persists in IndexedDB, keyed by the
same firmware hash the desktop build uses, so two dumps never collide;
*Settings ▸ Erase everything* clears it. The clock follows the computer's,
which a tab wants rather than a battery-backed time of its own.

The emulator runs in a Web Worker and draws through WebGL on an OffscreenCanvas
where the browser has one, so a slow frame stutters the game rather than
freezing the page. Browsers without OffscreenCanvas take posted frames and draw
them on the page instead.

## Building

SDL2 is the only dependency, and Dear ImGui is vendored, so there is no toolkit
to install and the interface is identical on every platform.

### Linux

```sh
sudo apt install build-essential cmake libsdl2-dev
make
```

### Windows

Under MSYS2's MINGW64 shell, or cross-compiled from Linux with mingw-w64 —
[src/platform/windows/README.md](src/platform/windows/README.md) covers both.

```sh
src/platform/windows/build.sh      # from Linux; fetches SDL2 for mingw
```

### Release binaries

```sh
make dist
```

Builds both targets statically into `dist/`. SDL still opens X11, Wayland and
the audio servers at runtime rather than linking them, so the Linux binary
needs nothing but the C library and stays portable across desktops.

## What is emulated

| Block | Notes |
| --- | --- |
| CPU | ARMv7E-M interpreter: Thumb-1/Thumb-2, IT blocks, DSP/SIMD, FPv5-D16 including the fixed-point VCVT forms, NVIC, SysTick, exceptions |
| Memory | ITCM, DTCM, AXI SRAM, SRAM1/2, SRD SRAM, backup SRAM, internal flash, memory-mapped external flash of either size |
| OCTOSPI | Memory-mapped reads plus indirect-mode MX25U commands; the JEDEC capacity byte follows the fitted part |
| OTFDEC | Real AES-128 keystream, region config, and the hardware key-CRC the firmware verifies |
| CRYP | AES-128 GCM with real GHASH, plus ECB, CBC and CTR, and the FIFO service interrupts the driver waits on |
| LTDC | Two layers, ARGB8888/RGB888/RGB565/ARGB1555/ARGB4444/L8/AL44/AL88, CLUT, alpha blending |
| DMA2D | Blits, blending, register-to-memory fills, automatic CLUT loading |
| DMA | DMA1/DMA2 streams, circular and double-buffer modes, peripheral-driven transfers |
| PWR | Standby entry and wakeup-pin reset — this is how the POWER button turns the device on |
| RTC | Battery-backed calendar driven from the LSE, alarms and wakeup timer |
| SAI | 48 kHz output derived from the real clock tree (PLL2P 12.288 MHz), fed by the audio DMA |
| Misc | RCC with PLL1/2/3 modelled, GPIO, EXTI, SYSCFG, timers, SPI, ADC, RNG |

The clock is battery-backed like the real unit: it starts from a factory 12:00,
keeps running while the emulator is closed, and survives the reset the POWER
button performs. Config ▸ *Sync clock with host time* sets it from the host
instead. The emulated unit always reports itself on the charger.

## Command line

The emulator doubles as a reverse-engineering instrument, which is how most of
the hardware above was worked out — the button pin maps and the CRYP interrupt
number were read out of the firmware with these.

```sh
gwemu --headless 800 --profile         # exact PC histogram
gwemu --headless 500 --trace           # instruction history, dumped on faults
gwemu --headless 500 --watch LO HI     # log writes into an address range
gwemu --headless 500 --logcall ADDR    # log calls to an address with arguments
gwemu --headless 500 --log-periph      # log unmapped peripheral accesses
gwemu --headless 500 --dump-fb out.ppm # capture the composed frame
gwemu --log FILE                       # windowed, with a per-second [perf] line
```

`gwemu --help` lists the rest. `tools/` holds the offline scripts: OTFDEC
decryption of a flash dump, the entropy search that recovered its parameters,
and a cross-reference search over the internal flash image.

## Licence

The emulator is under the MIT licence. It contains no Nintendo code or assets.
