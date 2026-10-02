# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

See also: `../md-microfirmware-template/CLAUDE.md` (the SidecarTridge microfirmware app template this project is based on — richer detail on the shared architecture, build flow, and working style; its `programming.md` documents the shared cartridge-region rules). Setup mode runs on that template's modules (at `6935f53`); what this app changes is listed in "Divergences from the template" below.

## Project overview

**md-rom-emulator** is the SidecarTridge Multi-device ROM Emulator: a microfirmware app that makes an RP2040 (Raspberry Pi Pico / Pico W) board emulate a cartridge ROM for Atari ST/STE/Mega computers. It has two coupled firmwares:

- `rp/` — RP2040-side firmware in C (pico-sdk, CMake).
- `target/atarist/` — Atari-side firmware in m68k assembly (`src/main.s`, `src/inc/`, `src/userfw.s`), built with vasm/vlink through the `stcmd` wrapper (AtariST toolkit Docker image).

`AGENTS.md` in the repo root contains additional agent rules and style details; follow it.

**Key coupling:** `target/atarist/build.sh` assembles the target firmware into `dist/FIRMWARE.IMG` (padded to 64KB), converts it with `firmware.py` into a C array header, and copies it to `rp/src/include/target_firmware.h`, which is compiled into the RP firmware. Any change under `target/` requires rebuilding the target so the embedded header stays in sync.

## Build commands

Prerequisites: `arm-none-eabi-*` toolchain, CMake 3.26+, Python, `stcmd` on PATH, git submodules initialized (`pico-sdk` 2.2.0, `pico-extras` sdk-2.2.0, `fatfs-sdk` v3.6.2, the template's — do not vendor them; their pins change only in a planned upgrade).

```sh
# Full build (target + rp + dist packaging)
./build.sh <pico|pico_w> <debug|release> <app_uuid_key>
# e.g. ./build.sh pico_w debug 44444444-4444-4444-8444-444444444444

# Target-only (regenerates rp/src/include/target_firmware.h); it runs from its
# own folder, and RELEASE_DATE fixes the cartridge header's date
(cd target/atarist && ./build.sh "$(pwd)" release)

# RP-only
cd rp && ./build.sh <board_type> <build_type>

# Day to day: build out of tree, flash through the probe, verify over SWD
tools/dev/flash.sh debug            # or release; --build-only to just build
```

Build-script caveats (from AGENTS.md — respect these):

- **Avoid running `./build.sh` or `rp/build.sh` unless asked**: they delete `build/` and `dist/`, and re-pin the submodule tags. For verification, use `tools/dev/flash.sh <debug|release> --build-only`: it builds out of tree in `tools/dev/builds/` with rp/build.sh's environment and build type, and never touches `rp/build` or the submodules. FatFs is configured by `rp/src/ff/ffconf.h` (it wins over the submodule's copy through a `BEFORE PRIVATE` include), so the submodules stay pristine.
- `BOARD_TYPE` (env, default `pico_w`) selects `BOARD_TYPE_PICO_W`/`BOARD_TYPE_PICO` macros; the CYW43/lwIP WiFi stack is linked only when the board supports it (`pico_w`). `APP_UUID_KEY` (env) becomes `CURRENT_APP_UUID_KEY`; without it CMake uses the `4444…` dev UUID, which must match an app registered by the Booster or `main.c` jumps back to the Booster. `APP_DOWNLOAD_HTTPS` (env, default `1`, as releases ship) builds HTTP and HTTPS downloads; `0` builds HTTP only, which CI keeps compiling (`flash.sh` puts it in `tools/dev/builds/<type>-http`).
- The RP firmware is compiled **CMake `Release`** for both build types; the build-type argument only controls `DEBUG_MODE` (UART debug output via `DPRINTF`) and artifact naming. `RP_CMAKE_BUILD_TYPE` overrides the CMake type (`MinSizeRel` to compare, `Debug` to step through), with a warning and the type in the build ID. mbedTLS stays `-Os` whatever the type. v2.1.1's Release trouble came from defects since fixed: core 0's stack ran into core 1's during downloads (deeper at `-O3`), and in ROM mode the Wi-Fi driver's background timer interrupt (`threadsafe_background`) hung the main loop; the stack has its own bank and guard now, and the driver is polled.
- Every build carries a build ID in flash (`release_build_id`, from `rp/src/build_id.cmake`: the commit, `-dirty.<hash>` for uncommitted changes, the CMake type when it is not Release, e.g. `+minsizerel`, then `+https`, `+debug`), and the ELF keeps its symbol table; `swd.py build-id` reads the ID off a running RP. Debug builds run the console at 921,600 baud.
- Versioning comes from `version.txt` at the root (copied into `rp/` and `target/` by the root script). `make tag` tags and pushes the current version.

## Lint / format

- `clang-tidy`: `tools/dev/tidy.sh [FILE ...]` (after `tools/dev/flash.sh <type> --build-only`) runs it with the root `.clang-tidy` over the app's own sources, or the files given; the build does not run it. The config skips `modernize-macro-to-enum` (the tools read the `#define`s) and `readability-implicit-bool-conversion` (in C, `!`, `&&` and comparisons are `int`), and lets `module_camelBack` function names through. The template's own files do not pass it; the app's files and the app's lines in template files do.
- Format: `cmake --build rp/build --target clang-format`, or `clang-format -i rp/src/<file>.c` for one file. `.clang-format`: 2-space indent, 80 columns, attached braces, left pointer alignment (`type* ptr`).
- Naming: functions/variables `camelBack` (functions commonly `module_camelBack`, e.g. `emul_start`, `term_printString`); fixed-width types for firmware interfaces.
- `compile_commands.json` exists after any CMake configure (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`): `tools/dev/builds/<type>/` for `flash.sh`'s builds, `rp/build/` for `rp/build.sh`'s.

## Tests and developer tools

- **Host tests** (`tests/host`, from md-framebuffer-template): `make -C tests/host test` compiles firmware units with the host compiler under ASan and UBSan and runs them in seconds; CI runs them on every PR. `test_layout.py` checks that the ST side (`main.s`) and the RP side (`rp/src/include`) agree on every shared offset and command. A `bug_*.c` test shows a defect the firmware still has (`make -C tests/host known-bugs` passes only while each one fails); rename it to `test_*.c` when the defect is fixed.
- **On the hardware** (`tools/dev/`, see its `README.md`): `flash.sh` builds and flashes, `console.py` captures the 921,600-baud console, `swd.py` reads and drives the running RP over SWD (`screen`, `text`, `heap`, `crash`, `select`, `key`, `window`, `gdb`, ...), `smoke.py` runs a whole session through the probe, `testserver.py` serves catalogs and downloads with every failure case, `hatari_check.py` runs cartridge images under Hatari on every TOS, `fill_card.py` puts test files on the card through the device's own downloads.
- **Rebooting the ST without hands**: `swd.py st-reset` writes the setup menu's reset command and the signature of the remote reset agent that the self-check cartridge (`tools/dev/selfcheck/`) leaves in the ST's RAM. The agent (ST, STE and Mega STE: on a TT or a Falcon the RAM it takes is in use, and the self-check does not install it) survives every reset that keeps the ST powered and works from GEM or any ROM that keeps TOS's interrupts; a game that takes them over, or DiagROM, still needs the reset button, and so does a cartridge that waits for a hardware reset (Ultimate Ripper's frozen screen: the agent's reset is a software one). `st-reset` says beforehand who is listening. `swd.py boot-break` stops the RP at source lines during boot to take a different path (e.g. v2.1.2's autorun) without a rebuild.
- Validation otherwise: both firmwares compile, and if `target/` or the protocol changed, `target_firmware.h` was regenerated. Anything bus-facing is checked on the hardware.

## Architecture

### Boot flow (RP2040 side)

`rp/src/main.c` overclocks the RP2040 (`RP2040_CLOCK_FREQ_KHZ`, ≥225MHz needed for remote commands), loads the global config (`gconfig.c`) and per-app settings (`aconfig.c`) from dedicated flash sectors, and jumps to the **Booster app** (a separate firmware resident at 0x10120000) if settings are missing. Otherwise it calls `emul_start()` in `rp/src/emul.c` — the app's real entry point and main loop.

### Runtime modes (chosen at boot in `emul_start` from the `MODE` app setting)

App settings keys live in `aconfig.h` (`FOLDER`, `EMULATED`, `MODE`, `HTTPS_CATALOG`; `HTTP_CATALOG` is read only by an HTTP-only build); WiFi credentials/mode are *global* settings (`gconfig`) owned by the Booster.

1. **ROM emulation** — `MODE` = `ROM_MODE_DIRECT` (0) or `ROM_MODE_DELAY` (1): the ROM staged in `ROM_TEMP` flash is DMA-copied into `ROM_IN_RAM` and PIO+DMA emulation starts. Delay/Ripper mode first waits for a SELECT press before starting emulation. While emulating, a SELECT press writes `MODE=ROM_MODE_SETUP` and resets the RP; a long press resets and erases flash (`select.c`).
2. **Setup/terminal** — `MODE` = `ROM_MODE_SETUP` (255): the embedded target firmware is served as the cartridge instead, and a command-driven terminal UI runs (`term.c`, command table in `emul.c`). Boot sequence: SD init → autorun check → menu loop; the Wi-Fi connect (Pico W, `network_wifiStaConnectStart()`/`…Poll()`) and the refresh of `<FOLDER>/roms.csv` (`download.c` + `httpc/`) run behind the menu from the main loop (`netPoll()`, `downloadsPoll()`), and the menu's status values follow them. The menu takes single keys, no RETURN (`term_setCommandLevel()`); `[S]ettings` takes typed lines (`put_str KEY VALUE`, `save`, ...) until `m`. Features: `[B]rowse` the ROMs on microSD (`/roms` by default) and `[D]ownload` from the catalog, both in one list screen driven by the cursor keys (`navlist.c`; `term_setKeyHandler()` hands it every key while it is open), where RETURN on a ROM's details selects it (`[B]`) or downloads it on a screen with its progress (`[D]`; ESC leaves it running behind the menu); settings editing, toggle Delay mode, launch (a screen with the write's progress), exit to desktop, return to Booster.

**The setup screens** (`ui.c`, after md-devops' menu): the terminal prints the words, at fixed rows with `term_printAt()` (a cursor move blanks the cell it leaves), and `ui.c` draws around them: the inverted title bar with the SD card's and Wi-Fi's icons, group boxes with their label on the top edge and an icon, progress bars, the strip on the 25th row (key hints, or the product line), and notices: two inverted rows in the small font with the warning icon (`term_recordAt()` keeps their words in the screen buffer for `swd.py text`); the menu's own go after 5 s. The menu's rows are one table of constants in `emul.c`. While the menu is as `menu()` drew it (`menuIntact`), a status change redraws its values and icons in place (7 ms, against 59 ms for the menu), and a list's cursor redraws two rows (4 ms); the selection is an inverted line, and the tools read it over SWD (`listNav`, `listFirstRow`).

**The catalog** (`catalog.c`) is the card's copy, read a page at a time: opening `[D]` indexes the file (4 bytes a page), a page reads its 16 names, and the chosen entry is read whole; the file's order is kept. A refresh replaces the card's copy only when it is complete. A ROM download is one at a time, built from the catalog's origin (after its redirects); it must be the catalog's size to a KB or it is deleted, and only a finished one becomes the selection, under the name it was saved as. Entries with a `..` segment, a `\` or a drive in the URL, or over 129 KB, are refused when chosen. Offline, `[D]` shows the card's copy with a notice and the reason; a debug build takes the Wi-Fi down and up with `swd.py app wifi 0|1`.

**The network comes and goes** (from md-devops' link supervisor, v1.1.0): while connected, the main loop watches the link (lwIP's link status for an ordinary disconnect; an ARP request for the gateway every 60 s for a silent one, three unanswered in a row); a lost link or a failed join is tried again in the background, 5 s after a loss and then with a backoff doubling to 60 s, never while a download runs, and the boot's attempts are followed by the same tries. A join that finds no network by that name is armed again every 2 s within its attempt (the driver does not retry by itself). The menu's line says "Wi-Fi lost; joining again." or "Did not connect; trying again."; a join refreshes the catalog. Debug builds: `swd.py app wifi 0|1`.

**The SD card comes and goes** (`sdcard.c`, from md-devops v1.1.0): the board has no card-detect, so while a card is mounted the main loop reads sector 0 every 2 s (FatFs serves its cache and never notices a pull), and a failed operation checks at once; while none is, it mounts every 2 s, forgetting the driver's card and the FatFs volume first. A card an RP restart left inside a multi-block write (SELECT during a download: the board has no chip-select line, `hw_config.c` `ss_gpio = -1`) answers `FR_NOT_READY`: 1,200 bytes of `0xFF`, the stop token `0xFD` and 1,200 more free it. The boot goes on without a card; the menu says "SD card: none. Put one in."; `[B]`, `[D]` and `[L]` refuse; a pull closes an open list, ends a launch or a download with "the SD card is gone", and a card put in gets the catalog. Debug builds: `swd.py app sd 0|1`.

**Launching a ROM** (`romstore.c`, called by `cmdLaunch` and the autorun in `emul.c`) checks the size before anything is erased (at most 128 KB of ROM, past a 4-byte zero header on STEEM `.stc` images), erases all of `ROM_TEMP`, programs the ROM a sector at a time with every 16-bit word byte-swapped (the Atari is big-endian), reads each sector back, and only then lets the caller save `EMULATED` and `MODE`: a write that fails or is cut short leaves the device in setup mode with its previous selection. The ST and SELECT are serviced between sectors; interrupts are off for one sector's erase or program (about 39 ms at most). `[B]rowse` keeps its pick in the settings in RAM until a launch saves it; long file names are opened by their 8.3 alias when the list's field can't hold them. Accepted extensions: `img`, `rom`, `stc`, `bin`. **Autorun:** if `<FOLDER>/.autorun` exists and names a ROM, setup mode writes it the same way, sets `MODE=ROM_MODE_DIRECT`, and blinks the LED, answering the ST and SELECT (a press restarts into the ROM) — used for diagnostic cartridges on machines with broken keyboards or screens.

### ROM bus emulation core (`romemul.c` + `romemul.pio`)

PIO state machines watch the Atari cartridge bus (16 address/data GPIOs multiplexed through latches starting at GPIO 6, READ/WRITE latch signals on GPIO 27/28, `!ROM4` on GPIO 22 and `!ROM3` on GPIO 26; see `constants.h`), and DMA channels serve 16-bit reads directly from RAM with no CPU involvement and no IRQ. Setup mode loads the template's 16-bit program, which serves ROM4 (`$FA0000`) from the lower 64 KB of `ROM_IN_RAM`, plus the command ring below. ROM mode loads `romemul_read_two_banks` (17 address bits, `romemul_initTwoBanks()`): ROM4 from the lower 64 KB and ROM3 (`$FB0000`) from the upper, and nothing else, because the user's ROM owns the whole window. Every DMA channel and state machine is claimed, never hard-coded, and `romemul_stop()` / `commemul_stop()` release the bus (state machines off, DMA aborted, the latch controls back at idle) before every reset and before the jump to Booster (`emul_quiesce()`). Before `[X]` jumps, the reset command stays in place until the ST has rebooted (its TOS reads the cartridge header); a launch instead holds it 500 ms and restarts the RP within the cartridge's own `PRE_RESET_WAIT` (about 2.4 s at 8 MHz), so TOS finds the new ROM. ROM mode is live 11.4 ms after reset (release) and records the boot race in RAM (`romModeLiveUs`, `romModeAccessBeforeLive`, `romModeFirstAccessUs`); a debug build also captures ROM3 reads in ROM mode, passively, so `swd.py ring` reads a test cartridge's reports. Hot paths are `__not_in_flash_func`.

### Setup-mode communication (Atari ⇄ RP2040)

- **Atari → RP:** the cartridge is read-only, so the target firmware "writes" by *reading* addresses in the ROM3 window (`$FB0000`). `commemul` captures every ROM3 address into a 4 KB DMA ring (the cartridge sends only keystrokes and its hello); `chandler_loop()` drains it through the `tprotocol` parser (header `0xABCD`, command id, payload size, payload, checksum), runs the registered callbacks (`term_command_cb` for the terminal), and answers at once by writing the command's token at `$FA2004` and a new seed at `$FA2008`, which the Atari polls. The ST's boot hello and its shared variables (machine, TOS version, from `$FA2010`) arrive the same way; on the hello the menu drops typed input, clears the sentinel (a command left from the last session, such as the desktop after `[E]`, sent every later reset there again) and redraws.
- **The poll tick:** `emul_pollTick()` runs `chandler_loop()`, `term_loop()` and `select_poll()`. The main loop and every long wait (the SD scan, the flash copy of a launch, the Booster handover) call it; the Wi-Fi connect and the downloads are polled from the main loop and never wait, so the ST is answered and SELECT is seen during them.
- **RP → Atari screen:** there is no text protocol. The RP renders a 320×200 monochrome framebuffer with u8g2 (`display.c`, `display_term.c`, VT52-subset terminal in `term.c`) *directly into ROM_IN_RAM* at `$FAE0C0` (the top of ROM4), and the target firmware copies it to video RAM (with a translation table at `$FA2100` for high resolution).
- **RP → Atari control:** the command sentinel at `$FA2000` carries display commands (`DISPLAY_COMMAND_RESET`, `…_CONTINUE` to boot GEM, `…_START` to jump to `userfw`); the target polls it. Anything the RP writes into shared memory must be word-swapped (`WRITE_AND_SWAP_LONGWORD`, `CHANGE_ENDIANESS_BLOCK16` in `memfunc.h`).
- The offsets live in `target/atarist/src/inc/sidecart_layout.s` and `rp/src/include/chandler.h` (and `display.h`); `tests/host/test_layout.py` checks that both sides agree. Changing one means changing both, then regenerating `target_firmware.h`.

### Device health (`health.c`, from md-devops v1.1.0)

`emul_start()` first decodes why the RP started, then arms the watchdog (8 s) in both modes. The record lives in watchdog scratch 0-3: magic, reason, crash count and window, the current phase (`health_setPhase()`: boot, the menu, the SD card, Wi-Fi connect, catalog download, ROM download, a ROM write, ROM mode, the Delay wait); PC, LR and SP at a crash. `panic()` goes through `PICO_PANIC_FUNCTION=health_panic`; a HardFault (the stack guard included) through a handler installed in the RAM vector table at boot (fatfs-sdk's stops at a breakpoint). Both record themselves and reboot; a hang lets the watchdog fire, and a stall mark set by a timer interrupt tells it from a probe's reset. `reset_device()` marks a reset the user asked for (SELECT, a launch); `reset_jump_to_booster()` stops the watchdog first, since Booster does not feed it. The main loop, `emul_pollTick()`, the ROM-mode and Delay waits and the service waits feed it; the longest gap measured is about 1 s (the boot, through the SD card), 55 ms through a launch (`healthMaxFeedGapUs`). Three crash reboots within 60 s boot setup mode with no autorun until a requested reset. The main menu shows "Last restart: hang in catalog download" (and `x3`) under the title after an unrequested reboot; `swd.py crash` decodes the record. Debug builds provoke each failure with `swd.py app health N` and a ROM-mode hang with `romModeTestHang`.

### Memory map (`rp/src/memmap_rp.ld` — custom linker script, load-bearing)

- Flash: app 1MB @0x10000000 · `ROM_TEMP` 128K @0x10100000 (staged ROM) · Booster app 768K @0x10120000 · config sectors + app lookup table at the top (0x101E0000+).
- RAM: `RAM` 128K @0x20000000 (static data, then the heap, which stops at `0x20020000`: `malloc` returns NULL, never memory in the window) · `ROM_IN_RAM` 128K @0x20020000 (2×64K banks served to the Atari; in setup mode the ROM3 bank, served to nobody, holds mbedTLS's allocations) · core 0's 4 KB stack in `SCRATCH_Y` with a stack guard (8 KB, `SCRATCH_X` too, in an HTTPS build). Core 1 never starts.

Changing this layout or flash usage risks clobbering the Booster app or config areas.

### Target firmware (`target/atarist/`)

The template's `src/main.s` and `src/inc/` (senders with retries, token and seed, the boot hello, the Mega STE cache), plus `src/userfw.s`, all built by `Makefile` via vasm/vlink inside `stcmd` → `BOOT.BIN` (the build stops past 8 KB of cartridge code) → padded to exactly 64KB → embedded into the RP firmware as the boot/menu ROM the Atari executes in setup mode.

### Support libraries in-tree

`rp/src/settings/` (flash-backed key/value settings) and `rp/src/httpc/` (lwIP HTTP client) are compiled into the firmware target, as in the template; `rp/src/u8g2/` (trimmed display library) is a subdirectory CMake library. None of them are submodules.

## Divergences from the template

Setup mode is the template's at `6935f53`; every other file under `rp/src` and `target/atarist` is byte-identical to it, apart from this app's own modules (`romstore.c`, `catalog.c`, `navlist.c`, `ui.c`) and their lines in `rp/src/CMakeLists.txt`. These differ:

- **`emul.c`, `emul.h`:** this app's menu (`[B]rowse`, `[D]ownload`, `[S]ettings`, `[R]ipper`, `[L]aunch`, `[E]xit`, `[X]` Booster) and ROM mode on the template's skeleton. No `[F]irmware`: a debug build has a hidden `f` command that starts `userfw` for `tools/dev/st_harness.py`.
- **`aconfig.*`:** this app's keys (`EMULATED`, `FOLDER`, `HTTP_CATALOG`, `HTTPS_CATALOG`, `MODE`), unchanged from v2.1.2.
- **`constants.h`, `romemul.*`, `memmap_rp.ld`:** two 64 KB banks (`ROM_BANKS`, the two-bank PIO program, `RAM` and `ROM_IN_RAM` 128 KB each).
- **`term.c`:** the cursor keys' scan codes become `TERM_KEY_UP`/`DOWN`/`LEFT`/`RIGHT` (md-drives-emulator's codes), and a key handler (`term_setKeyHandler()`) takes every keystroke while a list is open. Single keys at the menu and typed lines in the settings (`term_setCommandLevel()`, md-devops' levels, two of them); `APP_TERMINAL_START`, which `main.s` sends for the ST's ESC key, goes to an open list's key handler as ESC, and otherwise draws the menu (its `m`). `term_printAt()` and `term_recordAt()` write at a row and column without moving the cursor.
- **`download.c`:** the saved file's name has its `%XX` escapes decoded, so `Buggy%20Boy.img` is saved as `Buggy Boy.img`; `download_getBytesWritten()` for a download's progress.
- **`tprotocol.h`:** a plain-C payload store for host builds (`tests/host`); the RP still uses the `strh` asm.
- **`commemul.c`:** a `commemul_stop()`, which the template lacks.
- **`main.s`:** after copying its code to RAM, it clears a 68020's or 68030's instruction cache (the `_CPU` cookie: a TT, a Falcon) before running the copy; the senders' own clear needs the machine type, which is not known yet then. SHIFT does not leave the menu, as in the template. **`userfw.s`:** a stub that returns to TOS.
- **Fixes to the template's own code, reported upstream:** `term.c` drops a key typed during a long command instead of running it inside the command (`term_setBusy()` does the same for the autorun at boot), and `erase` reloads the defaults; `gconfig.c` passes its defaults table's size and `settings.c` reads stored entries up to the area's slots; `tprotocol.h`'s `MAX_PROTOCOL_PAYLOAD_SIZE` in parentheses; `chandler.c` counts the probe's injected commands apart (`chandlerInjected`) and does not answer them; `reset.c` prints its wait once; `commemul.c`'s 16 KB ring in its own section at the start of `RAM` (`memmap_rp.ld`); `download.c` refuses a download without a `FOLDER` setting; `main.s` rounds the RAM copy up to whole longs; `reset.h`'s jump to Booster takes VTOR's address in a register (its `ldr r1, =` left the constant to a literal pool out of reach in a Release debug build); `.clang-tidy` (see Lint).
- **`network.c`:** the STA connect without blocking (`network_wifiStaConnectStart()`/`…Poll()`, which arms the join again on "no network"; `network_wifiStaConnect()` is the two in a loop), the link check and the gateway probe (`network_isLinkHealthy()`, `network_pollGatewayProbe()`, from md-devops).
- **`sdcard.c`, this app's (from md-devops):** the presence check, the remount (`sdcard_pollRemount()`, `sdcard_checkPresence()`), and this board's stuck-card recovery (`sdcard_recoverAtBoot()`), which the template, on the same board, has not.
- **Device health, this app's (not in the template yet):** `health.c`/`health.h` from md-devops; `reset.c` marks a requested reset (`health_markReset()`); `reset.h`'s jump to Booster stops the watchdog; `CMakeLists.txt` sets `PICO_PANIC_FUNCTION`.
- **HTTPS downloads, built by default:** mbedTLS allocates in the ROM3 bank (`tlsArenaStart()` in `emul.c`, about 28 KB at the peak), installed once lwIP has created its shared TLS config, since lwIP points mbedTLS at its own allocator then; `mbedtls_config.h` keeps the AES tables in flash; `download.c` reads the response headers in place instead of copying them whole (GitHub's redirects send 5 KB of them); `commemul.c`'s ring is 4 KB; `CMakeLists.txt` guarantees an HTTPS build 29.5 KB of heap (18.6 KB at the peak over every test download, 35.4 KB in a release). `lwipopts.h` keeps the template's HTTPS sizes: 32 pool pbufs, 24 at the peak against an internet server. The HTTPS build reads `HTTPS_CATALOG`.

## Working style

Follow the "Working style" and "Editing guardrails" sections of `../md-microfirmware-template/CLAUDE.md`: think before coding, simplicity first, surgical changes, goal-driven execution. In particular:

- **Work on the hardware yourself, through the Debug Probe (hard rule).** Flash, reset, press SELECT (short and the 10 s factory-reset press), type keys, read the ST's screen as a PNG, the heap, the stack, the counters and the crash reason, and set breakpoints or step with GDB, over SWD (`tools/dev/swd.py`, OpenOCD, `arm-none-eabi-gdb`) and the debug UART (`tools/dev/console.py`). Until `tools/dev/` is in this repo, run the template's copy from `../md-microfirmware-template/tools/dev/`, which reaches the device only through the probe and the UART. Where no devhook exists yet, use GDB (write the variable, call the function). Check the probe and the console port first (`swd.py running`); if they're missing, that is the one thing to ask for.
- **Never use SWD or GDB while Booster runs** (only when developing Booster itself), and never jump to Booster (`[X]`) during a probe session. Every probe session starts from the **test microfirmware**, launched from Booster by Diego; our build is then flashed over it with `tools/dev/flash.sh ... --probe`. Booster's deploy API is not used: SWD and GDB only.
- **Ask Diego only for what needs hands**: swapping machines or SD cards, the ST's own keyboard inside a user's ROM, the reset button when nothing on the ST listens (`st-reset` says so), a real card pull, the Wi-Fi AP. Do all probe work first, then put every hands step in one session, planned up front, one short instruction at a time. Never ask him to press or type what the probe can.
- **No power-cycle series**: the machines are 40 years old. Reboot through the menu, `st-reset` or the agent; a single cold start only when the cold start itself is under test, asked first with the reason.
- **Don't commit, push or open a PR until Diego's go-ahead** at the end of the work; then do all three.
- **No AI attribution anywhere**: no "Generated with Claude Code", no `Co-Authored-By: Claude` trailers, no AI mentions in commits, PRs, comments, or docs. Write messages as the human author.
- Never modify the submodules (`pico-sdk/`, `pico-extras/`, `fatfs-sdk/`). Their pins change only in a planned upgrade, in `rp/build.sh`, `tools/dev/flash.sh` and the recorded commits together.
- Don't add features to `main.c` — feature work starts in `emul.c` or a new module.

## Releases / CI

- GitHub Actions `build.yml` builds `pico_w release` on PRs using the AtariST toolkit Docker image for `stcmd`.
- `release.yml` runs on `v*` tags: builds, creates the GitHub release, and uploads the `.uf2`/`.json` to S3. The release notes are the top of `CHANGELOG.md` up to the first `---` line, so new entries go at the top and end with `---`. `version.txt` holds the `v`-prefixed version that `make tag` (root Makefile) pushes.
- `upload_s3.sh` is the manual S3 publish path; it sources the untracked local `secrets.sh` for credentials — never commit or print it.
- Full-build artifacts land in `dist/`: `<APP_UUID>-<VERSION>.uf2`, `<APP_UUID>.json` (from the `desc/app.json` template), `rp.uf2.md5sum`.
