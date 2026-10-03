# AGENTS.md — ROM Emulator Playbook

**Read `CLAUDE.md` first.** It is the source of truth for what this repo is
(the SidecarTridge Multi-device ROM Emulator for the Atari ST, STE, Mega ST,
Mega STE, TT and Falcon), the build flow and its gotchas, the architecture
(ROM mode, setup mode, the command path, the flash and RAM layout), the
divergences from the microfirmware template, and the working style. This file
does not repeat any of it.

What lives here instead: getting a host machine set up, the commands worth
copy-pasting, a symptom → fix table for the failures this project actually
produces, the code style, and the hard rules.

## 1. Host environment setup

- **ARM GNU Toolchain 14.2** — `arm-none-eabi-gcc` on `PATH`, or
  `PICO_TOOLCHAIN_PATH` pointing at its `bin` directory.
- **CMake 3.26+** and **Python 3**.
- **`atarist-toolkit-docker`** (provides `stcmd`, needs Docker) — required for
  the m68k target. It shells out to `docker run -it`, so it needs a TTY unless
  `STCMD_NO_TTY=1` is set (`target/atarist/build.sh` sets it for you).
- **Git submodules initialized** (`pico-sdk` 2.2.0, `pico-extras` sdk-2.2.0,
  `fatfs-sdk` v3.6.2).
- **Raspberry Pi Debug Probe** — wired to the Multi-device header for SWD and
  the UART console (TX, RX **and both GND pins**), and **OpenOCD** (`$OPENOCD`,
  `openocd` on `PATH`, or `../pico/openocd/src/openocd`; its scripts from
  `$PICO_OPENOCD_PATH`). See `tools/dev/README.md`.

SDK paths — the build scripts set these from the repo if unset, so you only
need them for editor/IntelliSense integration:

```bash
export PICO_SDK_PATH=$REPO_ROOT/pico-sdk
export PICO_EXTRAS_PATH=$REPO_ROOT/pico-extras
export FATFS_SDK_PATH=$REPO_ROOT/fatfs-sdk
```

## 2. Common commands

```bash
# Verify a change: builds out of tree in tools/dev/builds/<type>, never
# touches rp/build or the submodules
tools/dev/flash.sh debug --build-only        # or release

# Build, flash (picotool or the Debug Probe) and check over SWD that the RP
# runs the new build
tools/dev/flash.sh debug

# Host tests: firmware units under ASan and UBSan, and the ST/RP shared layout
make -C tests/host test
make -C tests/host build/test_romstore && tests/host/build/test_romstore
(cd tests/host && python3 -m unittest test_layout)

# clang-tidy over the app's sources, after a --build-only; format one file
tools/dev/tidy.sh [FILE ...]
clang-format -i rp/src/<file>.c

# m68k target only: regenerates rp/src/include/target_firmware.h
(cd target/atarist && ./build.sh "$(pwd)" release)

# Full build (deletes build/ and dist/, re-pins the submodules): only when
# asked. Board type pico_w only.
./build.sh pico_w release 44444444-4444-4444-8444-444444444444

# The running RP, through the probe
python3 tools/dev/swd.py build-id
python3 tools/dev/swd.py text              # the ST's screen as text
python3 tools/dev/console.py tail          # the debug build's console log
```

A full build leaves `dist/<UUID>-<version>.uf2`, `dist/<UUID>.json` and
`dist/rp.uf2.md5sum`, and prints the MD5 written into the JSON.

## 3. Troubleshooting

| Symptom | Fix |
| --- | --- |
| `the input device is not a TTY` from `stcmd` | Export `STCMD_NO_TTY=1`. `target/atarist/build.sh` already does this; you only need it when calling `stcmd` yourself from a non-TTY context. |
| `arm-none-eabi-gcc not found` | The toolchain is not on `PATH`, or `PICO_TOOLCHAIN_PATH` does not point at its `bin` directory. |
| `ERROR: <script>: failed at line N` | The step on that line failed; the real error is printed just above it. Every build script stops at the first failure, and `dist/` is left empty. |
| `ERROR: cartridge code is N bytes; limit is 8192` | The m68k image outgrew its 8 KB budget. Trim `main.s`/`userfw.s`. |
| `ERROR: unknown build type` / `unknown board type` | The build type is `release` or `debug` (any case); the board type is `pico_w` (the app needs the Wi-Fi). |
| ``region `RAM' overflowed`` at link time | Static data leaves less than the heap the link guarantees (`APP_HEAP_SIZE` in `rp/src/CMakeLists.txt`: 27 KB with HTTPS, 32 KB HTTP only). Move big buffers to the heap or shrink them. |
| The app goes back to Booster at boot | Its `APP_UUID_KEY` is not an app Booster registered (the `4444…` development UUID included), or its settings are missing. |
| The RP restarts and the menu says "Last restart: ..." | The device health record caught a crash or a hang: `swd.py crash` decodes it (reason, phase, PC, LR, SP). A HardFault deep in app code is probably a stack overflow into the guard: `tools/dev/stackdepth.py` lists the big frames, `swd.py stack peak` the depth. |
| The ST shows garbage but terminal commands still work | `target_firmware.h` is stale: a `stcmd make` run by hand failed and an older `BOOT.BIN` survived, so the m68k runs against the wrong shared-region addresses. Rebuild the target. |
| Keys from the ST are dropped or arrive late | Some loop blocks without draining the ROM3 ring. Every blocking wait calls `emul_pollTick()` (see `CLAUDE.md`). |
| The RP freezes when a launch starts | A probe read of flash during the launch's ROM write stalls the bus. Leave the probe alone for about 8 s after `[L]`. |
| `swd.py key` / `type` do nothing | A release build has no devhooks; drive a debug build. |
| `swd.py st-reset` finds no listener | The self-check cartridge's reset agent is not in the ST's RAM (another ROM ran, or a TT or Falcon, where it is not installed). Launch `selfcheck.img` again, or use the reset button. |
| A `release` build prints nothing on serial | Expected: `DPRINTF` and UART stdio are compiled out unless the build type is `debug`. |
| `Unable to create '.../.git/modules/<submodule>/index.lock': File exists` | A git process died in that submodule earlier. If none is running, delete the lock and build again. |
| Local changes inside `pico-sdk/`, `pico-extras/` or `fatfs-sdk/` vanished | `rp/build.sh` re-checks-out the pinned revisions on every build. Never edit the submodules; FatFs config belongs in `rp/src/ff/ffconf.h`. |

## 4. Code style

Follow `.clang-format` and `.clang-tidy` in the repo root (`CLAUDE.md` > Lint).

- 2-space indent, 80 columns, attached braces, no tabs, left pointer alignment
  (`type* ptr`); clang-format orders the includes.
- Functions `camelBack`, commonly `module_camelBack` (`emul_start`,
  `term_printString`); variables and parameters `camelBack`; types
  `CamelCase`.
- Fixed-width types (`uint32_t`, `int16_t`) for firmware interfaces; `bool` for
  boolean logic; named constants instead of magic numbers (`0`, `1` and `-1`
  are allowed).
- Standard headers before project headers; keep headers minimal.
- Check return codes and NULL pointers from hardware and SDK calls, pass errors
  up rather than ignore them, and log them instead of asserting on runtime
  paths.
- Hot paths (anything the bus or a DMA completion waits on) are
  `__not_in_flash_func`.

## 5. Hard rules

Full guardrails and working style are in `CLAUDE.md`. The ones that are
non-negotiable and cheap to violate by accident:

- **Never modify `pico-sdk/`, `pico-extras/` or `fatfs-sdk/`.** They are pinned
  submodules and the build overwrites any local change. Their pins change only
  in a planned upgrade.
- **Don't run `./build.sh` or `rp/build.sh` unless asked**: they delete
  `build/` and `dist/` and re-pin the submodules. Verify with
  `tools/dev/flash.sh <type> --build-only`.
- **A change under `target/` needs the target rebuilt**, so
  `rp/src/include/target_firmware.h` matches; a shared offset or command
  changes on both sides, and `tests/host/test_layout.py` checks them.
- **Never use SWD or GDB while Booster runs**, and never jump to Booster
  (`[X]`) during a probe session.
- **Don't discard local changes, commit, push or open a PR** without the
  maintainer's go-ahead.
- **Never add AI-tool attribution** to commits, PR descriptions, code comments,
  docs, or any other artifact. No `Co-Authored-By: Claude …`, no "Generated
  with Claude Code / ChatGPT", no "AI-assisted" notes. Write everything as the
  human author.

## "Done" checklist

- `tools/dev/flash.sh release --build-only` and `debug --build-only` build, and
  `make -C tests/host test` passes.
- If `target/` changed, `rp/src/include/target_firmware.h` was regenerated.
- Anything bus-facing was checked on the hardware (`tools/dev/smoke.py` and the
  harnesses in `tools/dev/README.md`).

Keep this file updated as the process evolves so every agent starts with the
latest tribal knowledge.
