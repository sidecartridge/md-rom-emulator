# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `../md-microfirmware-template/CLAUDE.md` (the SidecarTridge microfirmware app template this project is based on — richer detail on the shared architecture, build flow, and working style; its `programming.md` documents the shared cartridge-region rules). **Caveat:** this repo forked from an *early* version of that template, so several template facts do not apply here — see "Divergences from the template" below.

## Project overview

**md-rom-emulator** is the SidecarTridge Multi-device ROM Emulator: a microfirmware app that makes an RP2040 (Raspberry Pi Pico / Pico W) board emulate a cartridge ROM for Atari ST/STE/Mega computers. It has two coupled firmwares:

- `rp/` — RP2040-side firmware in C (pico-sdk, CMake).
- `target/atarist/` — Atari-side firmware in m68k assembly (`src/main.s`), built with vasm/vlink through the `stcmd` wrapper (AtariST toolkit Docker image).

`AGENTS.md` in the repo root contains additional agent rules and style details; follow it.

**Key coupling:** `target/atarist/build.sh` assembles the target firmware into `dist/FIRMWARE.IMG` (padded to 64KB), converts it with `firmware.py` into a C array header, and copies it to `rp/src/include/target_firmware.h`, which is compiled into the RP firmware. Any change under `target/` requires rebuilding the target so the embedded header stays in sync.

## Build commands

Prerequisites: `arm-none-eabi-*` toolchain, CMake 3.26+, Python, `stcmd` on PATH, git submodules initialized (`pico-sdk`, `pico-extras`, `fatfs-sdk` — do not vendor or change their pins).

```sh
# Full build (target + rp + dist packaging)
./build.sh <pico|pico_w> <debug|release> <app_uuid_key>
# e.g. ./build.sh pico_w debug 44444444-4444-4444-8444-444444444444

# Target-only (regenerates rp/src/include/target_firmware.h)
./target/atarist/build.sh "$(pwd)/target/atarist" release

# RP-only
cd rp && ./build.sh <board_type> <build_type>
```

Build-script caveats (from AGENTS.md — respect these):

- **Avoid running `./build.sh` or `rp/build.sh` unless asked**: they delete `build/` and `dist/`, re-pin submodule tags (pico-sdk 2.1.0, pico-extras sdk-2.1.0, fatfs-sdk pinned commit), and patch `fatfs-sdk/src/include/ffconf.h` to enable `FF_USE_CHMOD`. For verification, compile directly (e.g. `cd rp/build && cmake ../src -DCMAKE_BUILD_TYPE=MinSizeRel && make -j4` with `PICO_SDK_PATH`, `PICO_EXTRAS_PATH`, `FATFS_SDK_PATH`, `BOARD_TYPE` set).
- The RP firmware is **always compiled `MinSizeRel`** regardless of the build-type argument — `Release` breaks (memory issues). The build-type argument only controls `DEBUG_MODE` (UART debug output via `DPRINTF`) and artifact naming.
- Versioning comes from `version.txt` at the root (copied into `rp/` and `target/` by the root script). `make tag` tags and pushes the current version.

## Lint / format

- `clang-tidy` runs automatically during the CMake build when installed; `.clang-tidy` at the root is the config. There is no standalone lint script.
- Format: `cmake --build rp/build --target clang-format`, or `clang-format -i rp/src/<file>.c` for one file. `.clang-format`: 2-space indent, 80 columns, attached braces, left pointer alignment (`type* ptr`).
- Naming: functions/variables `camelBack` (functions commonly `module_camelBack`, e.g. `emul_start`, `term_printString`); fixed-width types for firmware interfaces.
- `rp/build/compile_commands.json` exists after any CMake configure (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

## Tests

There is no first-party test suite (`tests/atarist` is empty). Validation = both firmwares compile, and if `target/` or the protocol changed, `target_firmware.h` was regenerated.

## Architecture

### Boot flow (RP2040 side)

`rp/src/main.c` overclocks the RP2040 (`RP2040_CLOCK_FREQ_KHZ`, ≥225MHz needed for remote commands), loads the global config (`gconfig.c`) and per-app settings (`aconfig.c`) from dedicated flash sectors, and jumps to the **Booster app** (a separate firmware resident at 0x10120000) if settings are missing. Otherwise it calls `emul_start()` in `rp/src/emul.c` — the app's real entry point and main loop.

### Two runtime modes (chosen at boot in `emul_start`)

1. **ROM emulation mode** — if a ROM is selected in settings: the ROM image staged in the `ROM_TEMP` flash region is copied into a dedicated RAM region, PIO+DMA emulation starts, and core logic just waits for the SELECT button (which reboots into setup / back to Booster).
2. **Setup/terminal mode** — no ROM selected (or SELECT pressed): runs a command-driven terminal UI (`term.c`, command table in `emul.c`) shown on the Atari via the embedded target firmware. Features: browse ROMs on microSD (FatFs, `sdcard.c`), download ROMs over WiFi (`network.c` + `httpc/` lwIP HTTP client, Pico W only), settings management, Delay/Ripper mode (`select.c` handles the physical SELECT button).

### ROM bus emulation core (`romemul.c` + `romemul.pio`)

PIO state machines watch the Atari cartridge bus (17 bus bits, ROM4/ROM3 selects) and DMA channels serve 16-bit reads directly from RAM with no CPU involvement. Time-critical handlers are `__not_in_flash_func`. The Atari→RP2040 command channel works the same way: the target firmware performs magic-address reads that the PIO/DMA path decodes as a protocol (`tprotocol.h`, header `0xABCD`) — the Atari "writes" by reading addresses.

### Memory map (`rp/src/memmap_rp.ld` — custom linker script, load-bearing)

- Flash: app 1MB @0x10000000 · `ROM_TEMP` 128K @0x10100000 (staged ROM) · Booster app 768K @0x10120000 · config sectors + app lookup table at the top (0x101E0000+).
- RAM: app half @0x20000000 · `ROM_IN_RAM` 128K @0x20020000 (2×64K banks served to the Atari).

Changing this layout or flash usage risks clobbering the Booster app or config areas.

### Target firmware (`target/atarist/`)

Single m68k source `src/main.s`, built by `Makefile` via vasm/vlink inside `stcmd` → `BOOT.BIN` → truncated/padded to exactly 64KB → embedded into the RP firmware as the boot/menu ROM the Atari actually executes in setup mode.

### Support libraries in-tree

`rp/src/settings/` (flash-backed key/value settings), `rp/src/httpc/` (lwIP HTTP client), `rp/src/u8g2/` (trimmed display library). These are subdirectory CMake libraries, not submodules.

## Divergences from the template

Where this repo differs from the current `md-microfirmware-template` (don't apply these template facts here):

- **No `userfw.s` / 8KB cartridge budget / `chandler.h`.** The target is a single `target/atarist/src/main.s` (plus `src/inc/` helpers); the only size check is the 64KB cap in `target/atarist/build.sh`.
- **Shared-region offsets differ:** framebuffer at `$FA8000` (not `$FAE0C0`), random token block at `$FAF000`. Offsets are defined in `main.s` and `rp/src/include/constants.h` — always use the named symbols.
- **No `rp/src/ff/ffconf.h` override.** FatFs config is patched *into the submodule* by `rp/build.sh` (sed enables `FF_USE_CHMOD` in `fatfs-sdk/src/include/ffconf.h`). A stray `ffconf.h.bak` at the root is a byproduct of that patch.
- **Submodule pins:** pico-sdk 2.1.0 / pico-extras sdk-2.1.0 (template uses 2.2.0).
- **lwIP mode:** `pico_cyw43_arch_lwip_threadsafe_background` (template uses poll mode).
- **No `tprotocol.c`** — only the header `rp/src/include/tprotocol.h` (macros).

## Working style

Follow the "Working style" and "Editing guardrails" sections of `../md-microfirmware-template/CLAUDE.md`: think before coding, simplicity first, surgical changes, goal-driven execution. In particular:

- **No AI attribution anywhere**: no "Generated with Claude Code", no `Co-Authored-By: Claude` trailers, no AI mentions in commits, PRs, comments, or docs. Write messages as the human author.
- Never modify the submodules (`pico-sdk/`, `pico-extras/`, `fatfs-sdk/`) or their pins.
- Don't add features to `main.c` — feature work starts in `emul.c` or a new module.

## Releases / CI

- GitHub Actions (`.github/workflows/build.yml`) builds `pico_w release` on PRs using the AtariST toolkit Docker image for `stcmd`.
- `upload_s3.sh` publishes `dist/` artifacts to S3; it sources the untracked local `secrets.sh` for credentials — never commit or print it.
- Full-build artifacts land in `dist/`: `<APP_UUID>-<VERSION>.uf2`, `<APP_UUID>.json` (from the `desc/app.json` template), `rp.uf2.md5sum`.
