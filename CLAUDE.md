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

# Day to day: build out of tree, flash through the probe, verify over SWD
tools/dev/flash.sh debug            # or release; --build-only to just build
```

Build-script caveats (from AGENTS.md — respect these):

- **Avoid running `./build.sh` or `rp/build.sh` unless asked**: they delete `build/` and `dist/`, re-pin submodule tags (pico-sdk 2.1.0, pico-extras sdk-2.1.0, fatfs-sdk pinned commit), and patch `fatfs-sdk/src/include/ffconf.h` to enable `FF_USE_CHMOD`. For verification, use `tools/dev/flash.sh <debug|release> --build-only`: it builds out of tree in `tools/dev/builds/` with rp/build.sh's environment and build type, and never touches `rp/build` or the submodules. A direct compile only links if that `FF_USE_CHMOD` patch is already present in the submodule (`download.c` calls `f_chmod`); it shows up as a local modification in `fatfs-sdk` — leave it in place.
- `BOARD_TYPE` (env, default `pico_w`) selects `BOARD_TYPE_PICO_W`/`BOARD_TYPE_PICO` macros; the CYW43/lwIP WiFi stack is linked only when the board supports it (`pico_w`). `APP_UUID_KEY` (env) becomes `CURRENT_APP_UUID_KEY`; without it CMake uses the `4444…` dev UUID, which must match an app registered by the Booster or `main.c` jumps back to the Booster.
- The RP firmware is **always compiled `MinSizeRel`** regardless of the build-type argument — `Release` breaks (memory issues). The build-type argument only controls `DEBUG_MODE` (UART debug output via `DPRINTF`) and artifact naming.
- Every build carries a build ID in flash (`release_build_id`, from `rp/src/build_id.cmake`: the commit, `-dirty.<hash>` for uncommitted changes, `+minsizerel` for the current build type, `+debug`), and the ELF keeps its symbol table; `swd.py build-id` reads the ID off a running RP. Debug builds run the console at 921,600 baud.
- Versioning comes from `version.txt` at the root (copied into `rp/` and `target/` by the root script). `make tag` tags and pushes the current version.

## Lint / format

- `clang-tidy` runs automatically during the CMake build when installed; `.clang-tidy` at the root is the config. There is no standalone lint script.
- Format: `cmake --build rp/build --target clang-format`, or `clang-format -i rp/src/<file>.c` for one file. `.clang-format`: 2-space indent, 80 columns, attached braces, left pointer alignment (`type* ptr`).
- Naming: functions/variables `camelBack` (functions commonly `module_camelBack`, e.g. `emul_start`, `term_printString`); fixed-width types for firmware interfaces.
- `rp/build/compile_commands.json` exists after any CMake configure (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`).

## Tests and developer tools

- **Host tests** (`tests/host`, from md-framebuffer-template): `make -C tests/host test` compiles firmware units with the host compiler under ASan and UBSan and runs them in seconds; CI runs them on every PR. `test_layout.py` checks that the ST side (`main.s`) and the RP side (`rp/src/include`) agree on every shared offset and command. A `bug_*.c` test shows a defect the firmware still has (`make -C tests/host known-bugs` passes only while each one fails); rename it to `test_*.c` when the defect is fixed.
- **On the hardware** (`tools/dev/`, see its `README.md`): `flash.sh` builds and flashes, `console.py` captures the 921,600-baud console, `swd.py` reads and drives the running RP over SWD (`screen`, `text`, `heap`, `crash`, `select`, `key`, `window`, `gdb`, ...), `smoke.py` runs a whole session through the probe, `testserver.py` serves catalogs and downloads with every failure case, `hatari_check.py` runs cartridge images under Hatari on every TOS, `fill_card.py` puts test files on the card through the device's own downloads.
- Validation otherwise: both firmwares compile, and if `target/` or the protocol changed, `target_firmware.h` was regenerated. Anything bus-facing is checked on the hardware.

## Architecture

### Boot flow (RP2040 side)

`rp/src/main.c` overclocks the RP2040 (`RP2040_CLOCK_FREQ_KHZ`, ≥225MHz needed for remote commands), loads the global config (`gconfig.c`) and per-app settings (`aconfig.c`) from dedicated flash sectors, and jumps to the **Booster app** (a separate firmware resident at 0x10120000) if settings are missing. Otherwise it calls `emul_start()` in `rp/src/emul.c` — the app's real entry point and main loop.

### Runtime modes (chosen at boot in `emul_start` from the `MODE` app setting)

App settings keys live in `aconfig.h` (`FOLDER`, `EMULATED`, `MODE`, `HTTP_CATALOG`); WiFi credentials/mode are *global* settings (`gconfig`) owned by the Booster.

1. **ROM emulation** — `MODE` = `ROM_MODE_DIRECT` (0) or `ROM_MODE_DELAY` (1): the ROM staged in `ROM_TEMP` flash is DMA-copied into `ROM_IN_RAM` and PIO+DMA emulation starts. Delay/Ripper mode first waits for a SELECT press before starting emulation. While emulating, a SELECT press writes `MODE=ROM_MODE_SETUP` and resets the RP; a long press resets and erases flash (`select.c`).
2. **Setup/terminal** — `MODE` = `ROM_MODE_SETUP` (255): the embedded target firmware is served as the cartridge instead, and a command-driven terminal UI runs (`term.c`, command table in `emul.c`). Boot sequence: SD init → autorun check → WiFi STA connect (Pico W) → download the ROM catalog CSV (`download.c` + `httpc/`) → menu loop. Features: browse ROMs on microSD (`/roms` by default), download ROMs to SD, settings editing, toggle Delay mode, launch, exit to desktop, return to Booster.

**Launching a ROM** (`storeFileToFlash` in `emul.c`) copies the file from SD into `ROM_TEMP` flash with every 16-bit word byte-swapped (the Atari is big-endian), skipping a 4-byte zero header on STEEM `.stc` images. Accepted extensions: `img`, `rom`, `stc`, `bin`. **Autorun:** if `<romsFolder>/.autorun` exists and names a ROM, setup mode flashes it, sets `MODE=ROM_MODE_DIRECT`, and blinks the LED forever (no reset) — used for diagnostic cartridges on machines with broken keyboards or screens.

### ROM bus emulation core (`romemul.c` + `romemul.pio`)

PIO state machines watch the Atari cartridge bus (16 address/data GPIOs multiplexed through latches starting at GPIO 6, READ/WRITE latch signals on GPIO 27/28, `!ROM4` on GPIO 22 and `!ROM3` on GPIO 26; see `constants.h`/`term.h`), and DMA channels serve 16-bit reads directly from RAM with no CPU involvement. ROM4 (`$FA0000`) is the first 64K bank of `ROM_IN_RAM`, ROM3 (`$FB0000`) the second. Time-critical handlers are `__not_in_flash_func`; never `DPRINTF` inside DMA IRQ callbacks.

### Setup-mode communication (Atari ⇄ RP2040)

- **Atari → RP:** the cartridge is read-only, so the target firmware "writes" by *reading* addresses in the ROM3 window (`$FB0000`). `term_dma_irq_handler_lookup` feeds each address to the `tprotocol.h` parser (header `0xABCD`, command id, payload size, payload, checksum) and only stashes the finished command; `term_loop()` (called from the main loop) handles it (`APP_TERMINAL` start/keystroke) and acknowledges by writing the command's random token at `$FAF000`, which the Atari polls; shared variables start at `$FAF200`.
- **RP → Atari screen:** there is no text protocol. The RP renders a 320×200 monochrome framebuffer with u8g2 (`display.c`, `display_term.c`, VT52-subset terminal in `term.c`) *directly into ROM_IN_RAM* at `$FA8000`, and the target firmware copies it to video RAM (with a translation table at `$FA1000` for high-res).
- **RP → Atari control:** a longword at framebuffer + 8000 carries display commands (`DISPLAY_COMMAND_RESET`, `…_CONTINUE` to boot GEM); the target polls it. Anything the RP writes into shared memory must be word-swapped (`WRITE_AND_SWAP_LONGWORD`, `CHANGE_ENDIANESS_BLOCK16` in `memfunc.h`).
- Changing any of these offsets requires changing both `target/atarist/src/main.s` and the RP headers (`term.h`, `display.h`, `constants.h`), then regenerating `target_firmware.h`.

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

- **Work on the hardware yourself, through the Debug Probe (hard rule).** Flash, reset, press SELECT (short and the 10 s factory-reset press), type keys, read the ST's screen as a PNG, the heap, the stack, the counters and the crash reason, and set breakpoints or step with GDB, over SWD (`tools/dev/swd.py`, OpenOCD, `arm-none-eabi-gdb`) and the debug UART (`tools/dev/console.py`). Until `tools/dev/` is in this repo, run the template's copy from `../md-microfirmware-template/tools/dev/`, which reaches the device only through the probe and the UART. Where no devhook exists yet, use GDB (write the variable, call the function). Check the probe and the console port first (`swd.py running`); if they're missing, that is the one thing to ask for.
- **Never use SWD or GDB while Booster runs** (only when developing Booster itself), and never jump to Booster (`[X]`) during a probe session. Every probe session starts from the **test microfirmware**, launched from Booster by Diego; our build is then flashed over it with `tools/dev/flash.sh ... --probe`. Booster's deploy API is not used: SWD and GDB only.
- **Ask Diego only for what needs hands**: a cold power cycle of the ST (it powers the RP), swapping machines or SD cards, the ST's own keyboard inside a user's ROM, a real card or power pull, the Wi-Fi AP. Put all of them in one list, once, with the fewest cycles that answer the question. Never ask him to press or type what the probe can.
- **Don't commit, push or open a PR until Diego's go-ahead** at the end of the work; then do all three.
- **No AI attribution anywhere**: no "Generated with Claude Code", no `Co-Authored-By: Claude` trailers, no AI mentions in commits, PRs, comments, or docs. Write messages as the human author.
- Never modify the submodules (`pico-sdk/`, `pico-extras/`, `fatfs-sdk/`) or their pins.
- Don't add features to `main.c` — feature work starts in `emul.c` or a new module.

## Releases / CI

- GitHub Actions `build.yml` builds `pico_w release` on PRs using the AtariST toolkit Docker image for `stcmd`.
- `release.yml` runs on `v*` tags: builds, creates the GitHub release, and uploads the `.uf2`/`.json` to S3. The release notes are the top of `CHANGELOG.md` up to the first `---` line, so new entries go at the top and end with `---`. `version.txt` holds the `v`-prefixed version that `make tag` (root Makefile) pushes.
- `upload_s3.sh` is the manual S3 publish path; it sources the untracked local `secrets.sh` for credentials — never commit or print it.
- Full-build artifacts land in `dist/`: `<APP_UUID>-<VERSION>.uf2`, `<APP_UUID>.json` (from the `desc/app.json` template), `rp.uf2.md5sum`.
