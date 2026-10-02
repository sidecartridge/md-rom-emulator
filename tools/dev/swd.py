#!/usr/bin/env python3
"""
swd — read, check and drive a running RP2040 through the Debug Probe.

Single-file, stdlib-only (Python >= 3.10). Runs OpenOCD with the CMSIS-DAP
probe for each command. It never uses the firmware's own services: memory is
read through the debug port while the CPU keeps running, so it works for any
microfirmware from the template and on a hung RP. Only `key`, `app` and
`inject` need firmware help, the debug-only mailbox in devhooks.h.

Usage:
    python3 tools/dev/swd.py running ELF [--timeout S]
    python3 tools/dev/swd.py verify ELF
    python3 tools/dev/swd.py build-id [ELF ...]
    python3 tools/dev/swd.py read ADDRESS LENGTH OUTFILE
    python3 tools/dev/swd.py program ELF
    python3 tools/dev/swd.py resume
    python3 tools/dev/swd.py reset
    python3 tools/dev/swd.py screen OUT.png [--elf ELF] [--scale N]
    python3 tools/dev/swd.py window FILE [--no-flash] [--elf ELF]
    python3 tools/dev/swd.py st-reset [--wait S] [--rom] [--offset N] [--elf ELF]
    python3 tools/dev/swd.py debug-pause on|off
    python3 tools/dev/swd.py stack paint|peak
    python3 tools/dev/swd.py boot-break STOP [STOP ...] [--via SYMBOL]
                                        [--timeout S] [--elf ELF]
    python3 tools/dev/swd.py shared [--elf ELF] [--all]
    python3 tools/dev/swd.py text [--elf ELF]
    python3 tools/dev/swd.py select short|long|release [--hold-ms MS] [--force]
    python3 tools/dev/swd.py key CHAR [--shift] [--scan N] [--elf ELF]
    python3 tools/dev/swd.py app NAME [--elf ELF]
    python3 tools/dev/swd.py inject COMMAND_ID [WORD ...] [--elf ELF]
    python3 tools/dev/swd.py crash [--elf ELF]
    python3 tools/dev/swd.py postmortem [--elf ELF] [--leave-halted]
    python3 tools/dev/swd.py gdb [--script FILE ...] [--seconds S] [--elf ELF]
    python3 tools/dev/swd.py heap [--elf ELF] [--watch SECONDS] [--csv FILE]
    python3 tools/dev/swd.py ring [--mark | --since-mark] [--elf ELF]

`running` waits until the vector table register (VTOR) holds the ELF's RAM
vector table, which the SDK's runtime init installs, and core 0 is not halted:
the RP has booted a firmware with that layout and got past its startup code.
`resume` releases both cores after a debugger left them halted. `verify`
compares the whole flashed image with the ELF. `build-id` reads the
`release_build_id` string from flash; with no ELF it tries every ELF in
tools/dev/builds/elf. `program` flashes the ELF and resets the RP; `reset` only
resets it. Both reset the whole chip through the watchdog, never with OpenOCD's
`reset` (see chip_reset).

`screen` renders the 320x200 framebuffer of the 64 KB cartridge window
(DISPLAY_BUFFER_OFFSET in display.h, CHANDLER_FRAMEBUFFER_OFFSET in chandler.h) as a PNG: the setup menu as the ST shows
it. `shared` prints the random token, the token seed and the indexed shared
variables, named after the `*_SVAR_*` / `*_SHARED_VARIABLE_*` indexes in
rp/src/include. Both find the window from the ELF's `__rom_in_ram_start__`;
without `--elf` they use the cached ELF whose build ID matches the RP.

`text` prints the terminal's character buffer (the `screen` array of term.c),
which is the setup menu as text; the bottom status line is drawn straight to
the framebuffer and only shows in `screen`.

`ring` decodes the commands the ST sent from the ROM3 capture ring
(commemul.c), without halting and on a release build as on a debug one: the
last 8,192 samples (about 800 small commands), oldest first. Each frame shows its
sample number, the command (named from the APP_<APP> / APP_<APP>_<COMMAND>
defines and chandler's framework commands in rp/src/include), the payload
size, the random token, the first 32-bit parameters in the order the RP's
TPROTO_GET_PAYLOAD_PARAM32 reads them, and whether its checksum holds.
`--mark` remembers the position (in tools/dev/logs/ring.mark), and
`--since-mark` shows only what arrived after it. Commands sent with `key` or
`inject` go through the mailbox, not the bus, so they never show here.

`select` presses the SELECT button: it forces the pin's input high through the
GPIO input override (IO_BANK0 GPIOn_CTRL.INOVER) for the hold time, so the
firmware sees a real press without any code of its own. `short` holds 300 ms,
`long` holds SELECT_LONG_RESET + 1 s (rp/src/include/select.h) and needs
`--force`, because in this firmware it erases the app's saved settings
(reset_deviceAndEraseFlash). `release` clears a stuck override.

`key`, `app` and `inject` need a debug build: they write the debug mailbox
(rp/src/include/devhooks.h) and wait until the main loop acknowledges it.
`key` sends a keystroke as the ST would, `inject` any protocol command with the
given 16-bit payload words after the random token, and `app` an app command
named by a DEVHOOKS_APP_<NAME> define in rp/src/include, with optional 16-bit
payload words, for example `app my_command 16`. The template defines none: an
app adds its own, and registers the handler with devhooks_setAppHandler().

`boot-break` restarts the RP through OpenOCD's `reset halt` and stops core 0
at each STOP on the way up, in order (FILE:LINE, the first place the line was
compiled into, or 0xADDRESS, then @REG=VALUE,... to set there), then lets it
run on: a boot path taken differently without a rebuild. On v2.1.2,
`boot-break emul.c:984@r7=255 0x20000310@pc=0x20000314,r0=4 --park-core1`
boots a ROM-mode device into setup mode and steps over the autorun's f_open
call as if it found no file, so a card with a .autorun still reaches the menu.
Step over the call: faking its result after it ran leaves the file open, and
FatFs then refuses to replace it. The addresses and registers are the
build's: read them from `arm-none-eabi-objdump -dl`.

`crash` explains the last reboot without stopping the RP: the watchdog reason
and scratch registers and, when the ELF has them, the record health.c decoded
at boot (bootCause, bootPhase, bootPc, bootLr, bootSp, as in md-devops), with
code addresses resolved to source lines.
`postmortem` halts the RP and prints both cores' backtraces, the registers, the
watchdog registers and key variables through GDB
($ARM_GDB_PATH/bin/arm-none-eabi-gdb or arm-none-eabi-gdb), then resumes it
unless --leave-halted. Halting stops the cartridge bus and pauses the RP2040's
timer until the RP resumes, so the ST sees a dead cartridge meanwhile.

OpenOCD is $OPENOCD, `openocd` on PATH, or ../pico/openocd/src/openocd next to
the repo; its scripts come from $PICO_OPENOCD_PATH (as in .vscode/launch.json),
else the tcl/ folder of a source build.

Exit codes:
    0  success
    1  generic / unexpected, or OpenOCD failed
    2  argparse usage error
    3  check failed: not running the ELF, image differs, or ID not found
"""

from __future__ import annotations

import argparse
import functools
import ast
import glob
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INCLUDE_DIR = os.path.join(REPO, "rp", "src", "include")
FB_WIDTH, FB_HEIGHT = 320, 200
VTOR = 0xE000ED08
DHCSR = 0xE000EDF0
DHCSR_S_HALT = 1 << 17
DHCSR_RELEASE = 0xA05F0000  # DBGKEY; clears C_HALT and C_DEBUGEN
CORES = ("rp2040.core0", "rp2040.core1")
IO_BANK0 = 0x40014000
INOVER_SHIFT = 16
INOVER_HIGH = 3
SELECT_SHORT_MS = 300
# DevhooksMailbox (rp/src/include/devhooks.h): offsets of its fields.
MAILBOX_SYMBOL = "devhooksMailbox"
MAILBOX_MAGIC = 0x444B4831
MB_SEQ, MB_ACK, MB_KIND, MB_RESULT, MB_CMD, MB_SIZE, MB_PAYLOAD = (
    4, 8, 12, 16, 20, 22, 24)
MAILBOX_WORDS = 16
KIND_PROTOCOL, KIND_APP = 1, 2
WATCHDOG_REASON = 0x40058008
WATCHDOG_SCRATCH0 = 0x4005800C
GDB_PORT = 3333
# Variables postmortem prints when the ELF has them (emul.c, chandler.c,
# commemul.c); a build without one simply lacks it. Add the app's own here.
# The command channel's counters (chandler.c, commemul.c), plain globals that
# `counters` reads without halting and `postmortem` prints.
COUNTERS = ("chandlerHandled", "chandlerDropped", "chandlerRepeated",
            "chandlerChecksumErrors", "commOverruns", "chandlerBusyUs",
            "chandlerMaxBusyUs", "chandlerGapUs", "chandlerMaxGapUs",
            "chandlerQuietUs", "chandlerMaxQuietUs", "chandlerFramePolls",
            "chandlerPollUs")
# Counters a build may lack (added later): read when the ELF has them.
OPTIONAL_COUNTERS = ("chandlerInjected",)
POSTMORTEM_VARIABLES = ("keepActive", "menuScreenActive", "protocolPending",
                        "incrementalCmdCount", "commReadIdx") + COUNTERS + (
    # health.c: why this boot happened, the crash-loop guard, the watchdog.
    "bootCause", "bootPhase", "bootPc", "bootLr", "bootSp", "crashCount",
    "feedCount", "stallMs", "healthMaxFeedGapUs", "healthMaxFeedGapPhase",
    "heapMinFree", "sbrkHighWater")
BUILD_ID_SYMBOL = "release_build_id"
# The ROM3 capture ring (commemul.c): its DMA channel's registers, and where
# `ring --mark` keeps its position. RING_PARAMS 32-bit parameters are printed.
COMMEMUL_C = os.path.join(REPO, "rp", "src", "commemul.c")
DMA_BASE, DMA_CHANNEL_STRIDE, DMA_WRITE_ADDR, DMA_CHANNELS = (
    0x50000000, 0x40, 0x04, 12)
RING_MARK = os.path.join(HERE, "logs", "ring.mark")
RING_PARAMS = 4
# The shared variables are one block of CHANDLER_SHARED_VARIABLES_SLOTS indexed
# 4-byte slots (chandler.h); an index past it is not a shared variable.
SVAR_MAX_SLOTS = 60


class SwdError(Exception):
    pass


def openocd_command(work_area: bool = False) -> list[str]:
    ocd = os.environ.get("OPENOCD") or shutil.which("openocd")
    if not ocd:
        local = os.path.join(REPO, "..", "pico", "openocd", "src", "openocd")
        if os.access(local, os.X_OK):
            ocd = local
    if not ocd:
        raise SwdError("no openocd: set OPENOCD")
    cmd = [ocd]
    scripts = os.environ.get("PICO_OPENOCD_PATH") or os.path.join(
        os.path.dirname(os.path.realpath(ocd)), "..", "tcl")
    if os.path.isfile(os.path.join(scripts, "interface", "cmsis-dap.cfg")):
        cmd += ["-s", scripts]
    cmd += ["-f", "interface/cmsis-dap.cfg", "-f", "target/rp2040.cfg",
            "-c", "adapter speed 5000"]
    # OpenOCD's rp2040.cfg puts its work area, where it loads routines such as
    # verify_image's CRC and the flash-size probe of a GDB connect, at
    # 0x20010000: inside the firmware's live RAM. After flashing, the CRC
    # routine was found there, over the Wi-Fi driver's async context (its
    # first word, 0x20004602, is the routine's first two instructions), and
    # the next cyw43_arch_poll() HardFaulted. Every run that does not write
    # flash gets a 4 KB work area in SCRATCH_X instead, which is core 1's
    # stack in a firmware that never starts core 1 (core 0's stack guard is at
    # the bottom of SCRATCH_Y, above it), backed up and restored. In a build
    # with HTTPS downloads core 0's stack runs on down through SCRATCH_X; the
    # work area is only used with the cores halted, and the backup puts the
    # stack back. Flash writes keep the default: they halt the cores first and
    # reset the chip after.
    if not work_area:
        cmd += ["-c", "rp2040.core0 configure -work-area-phys 0x20040000 "
                      "-work-area-size 0x1000 -work-area-backup 1"]
    return cmd


# The debug port can drop for a moment, for example while the firmware changes
# the clock and core voltage early in boot. OpenOCD then reports one of these.
TRANSIENT = re.compile(r"Failed to read memory|Error connecting DP|"
                       r"Examination failed|DP initialisation failed")
ATTEMPTS = 4


TCL_PORT = 6666


def tcl_server() -> socket.socket | None:
    """A running OpenOCD's TCL server (`swd.py gdb` opens one), or None.
    Only one program can hold the probe: while GDB is attached, the other
    commands go through the OpenOCD it uses."""
    try:
        sock = socket.create_connection(("127.0.0.1", TCL_PORT), timeout=0.3)
    except OSError:
        return None
    sock.settimeout(60)
    return sock


def tcl(sock: socket.socket, command: str) -> str:
    sock.sendall(command.encode() + b"\x1a")
    data = b""
    while not data.endswith(b"\x1a"):
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return data.rstrip(b"\x1a").decode(errors="replace")


def openocd(*commands: str, check: bool = True, work_area: bool = False) -> str:
    """Run OpenOCD with `init`, the commands and `exit`; return its output.
    A run that failed on a transient debug-port error is repeated. Only a run
    that writes flash, with the cores halted, asks for OpenOCD's default work
    area (see openocd_command). When an OpenOCD is already running with its
    TCL server (a `swd.py gdb` session), the commands go to it instead."""
    sock = tcl_server()
    if sock is not None:
        with sock:
            out = "\n".join(tcl(sock, "capture {" + c + "}") for c in commands)
            if any(c.startswith("targets ") for c in commands):
                tcl(sock, "targets " + CORES[0])
        if check and re.search(r"^Error", out, re.M):
            raise SwdError("openocd failed: " + out.strip()[-300:])
        return out
    args = openocd_command(work_area) + ["-c", "init"]
    for c in commands:
        args += ["-c", c]
    args += ["-c", "exit"]
    for attempt in range(ATTEMPTS):
        proc = subprocess.run(args, capture_output=True, text=True)
        out = proc.stdout + proc.stderr
        failed = proc.returncode != 0 or "Failed to read memory" in out
        if not (failed and TRANSIENT.search(out)) or attempt == ATTEMPTS - 1:
            break
        time.sleep(0.5)
    if check and proc.returncode != 0:
        lines = [l for l in out.splitlines()
                 if l.startswith("Error") and "algo" not in l]
        raise SwdError("openocd failed: " + (" / ".join(lines[-3:]) or
                                             out.strip()[-300:]))
    return out


def read_word(address: int, core: str = CORES[0]) -> int:
    out = openocd(f"targets {core}", f"mdw 0x{address:08x}")
    m = re.search(rf"0x{address:08x}:\s+([0-9a-fA-F]{{8}})", out)
    if not m:
        raise SwdError(f"cannot read 0x{address:08x}")
    return int(m.group(1), 16)


def read_memory(address: int, length: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "dump.bin")
        openocd(f"dump_image {path} 0x{address:08x} {length}")
        with open(path, "rb") as f:
            data = f.read()
    if len(data) != length:
        raise SwdError(f"read {len(data)} of {length} bytes")
    return data


@functools.lru_cache(maxsize=4)
def elf_nm(elf: str, mtime: float) -> str:
    nm = shutil.which("arm-none-eabi-nm")
    if not nm:
        raise SwdError("arm-none-eabi-nm not on PATH")
    return subprocess.run([nm, "-S", elf], capture_output=True, text=True,
                          check=True).stdout


def elf_symbols(elf: str, *names: str) -> dict[str, tuple[int, int]]:
    """Address and size of each named symbol present in the ELF."""
    out = elf_nm(os.path.abspath(elf), os.path.getmtime(elf))
    found = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in names:
            found[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
        elif len(parts) == 3 and parts[2] in names:
            found.setdefault(parts[2], (int(parts[0], 16), 0))
    return found


def cmd_running(args: argparse.Namespace) -> int:
    # boot2 points VTOR at the flash vector table before the firmware starts,
    # at the same address in every build; the SDK's runtime init then moves it
    # to RAM. Only the RAM table proves this firmware got past its startup.
    syms = elf_symbols(args.elf, "ram_vector_table", "__vectors")
    table = syms.get("ram_vector_table") or syms.get("__vectors")
    if not table:
        raise SwdError(f"{args.elf} has no vector table symbol")
    tables = {table[0]}
    deadline = time.monotonic() + args.timeout
    last = "no answer"
    while True:
        try:
            vtor = read_word(VTOR)
            halted = read_word(DHCSR) & DHCSR_S_HALT
            if vtor in tables and not halted:
                print(f"running: VTOR 0x{vtor:08x}")
                return 0
            last = f"VTOR 0x{vtor:08x}" + (", core 0 halted" if halted else "")
        except SwdError as exc:
            last = str(exc)
        if time.monotonic() >= deadline:
            print(f"not running {args.elf} after {args.timeout:g} s ({last})",
                  file=sys.stderr)
            return 3
        time.sleep(0.5)


def cmd_verify(args: argparse.Namespace) -> int:
    out = openocd(f"verify_image {args.elf}", check=False)
    m = re.search(r"verified (\d+) bytes in ([\d.]+)s", out)
    if m:
        print(f"flash matches {args.elf} ({m.group(1)} bytes)")
        return 0
    diffs = len(re.findall(r"^diff \d+ address", out, re.M))
    if diffs:
        print(f"flash differs from {args.elf} (at least {diffs} bytes)",
              file=sys.stderr)
        return 3
    raise SwdError("verify failed: " + out.strip()[-300:])


def read_build_id(elf: str) -> str | None:
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym or sym[1] == 0:
        return None
    data = read_memory(sym[0], sym[1])
    return data.split(b"\0", 1)[0].decode("ascii", errors="replace")


def cmd_build_id(args: argparse.Namespace) -> int:
    elfs = args.elf or sorted(
        glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
        key=os.path.getmtime, reverse=True)
    tried = set()
    for elf in elfs:
        sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
        if not sym or sym in tried:
            continue
        tried.add(sym)
        build_id = read_build_id(elf)
        expected = elf_build_id(elf)
        if build_id and (args.elf or build_id == expected):
            print(build_id)
            return 0
    print("no build ID found at the release_build_id address of "
          f"{len(tried)} ELF layout(s)", file=sys.stderr)
    return 3


def elf_build_id(elf: str) -> str | None:
    """The build ID string stored in the ELF file itself."""
    sym = elf_symbols(elf, BUILD_ID_SYMBOL).get(BUILD_ID_SYMBOL)
    if not sym:
        return None
    headers = subprocess.run(["arm-none-eabi-objdump", "-h", elf],
                             capture_output=True, text=True, check=True).stdout
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                         headers, re.M):
        name, size, vma = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if vma <= sym[0] < vma + size:
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "section.bin")
                subprocess.run(["arm-none-eabi-objcopy", "-O", "binary",
                                f"--only-section={name}", elf, path],
                               check=True)
                with open(path, "rb") as f:
                    f.seek(sym[0] - vma)
                    data = f.read(sym[1])
            return data.split(b"\0", 1)[0].decode("ascii", errors="replace")
    return None


def header_defines(*paths: str) -> dict[str, int]:
    """Integer #defines of one or more C headers, resolving references between
    them. Pass several headers when an index is built on a define from another
    header (the drivers' shared-variable indexes chain across headers here)."""
    raw: dict[str, str] = {}
    for header in paths:
        with open(header, encoding="utf-8", errors="replace") as f:
            text = f.read()
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", " ", text).replace("\\\n", " ")
        raw.update(re.findall(
            r"^\s*#\s*define\s+([A-Za-z_]\w*)[ \t]+([^\n]+)$", text, re.M))
    path = paths[0] if paths else "<none>"
    values: dict[str, int] = {}

    def resolve(name: str, depth: int = 0) -> int | None:
        if name in values:
            return values[name]
        if name not in raw or depth > 20:
            return None
        expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", raw[name])
        # Drop C casts like (uint32_t)(...): the value is what matters here.
        expr = re.sub(r"\(\s*(?:u?int\d+_t|unsigned|signed|int|long|short|"
                      r"size_t|char)\s*\)", " ", expr)
        for ref in set(re.findall(r"\b[A-Za-z_]\w*\b", expr)):
            v = resolve(ref, depth + 1)
            if v is None:
                return None
            expr = re.sub(rf"\b{ref}\b", str(v), expr)
        try:
            tree = ast.parse(expr.strip(), mode="eval")
        except SyntaxError:
            return None
        allowed = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant,
                   ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div,
                   ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd, ast.USub,
                   ast.Invert)
        if not all(isinstance(n, allowed) for n in ast.walk(tree)):
            return None
        v = eval(compile(tree, path, "eval"), {"__builtins__": {}})
        if not isinstance(v, (int, float)):
            return None
        values[name] = int(v)
        return values[name]

    for name in raw:
        resolve(name)
    return values


def matching_elf(explicit: str | None) -> str:
    """The ELF given, or the cached ELF whose build ID the RP carries."""
    if explicit:
        return explicit
    elfs = sorted(glob.glob(os.path.join(HERE, "builds", "elf", "*.elf")),
                  key=os.path.getmtime, reverse=True)
    for elf in elfs:
        expected = elf_build_id(elf)
        if expected and read_build_id(elf) == expected:
            return elf
    raise SwdError("no cached ELF matches the RP's build ID: pass --elf")


def cartridge_window(elf: str) -> int:
    base = elf_symbols(elf, "__rom_in_ram_start__").get("__rom_in_ram_start__")
    if not base:
        raise SwdError(f"{elf} has no __rom_in_ram_start__")
    return base[0]


def write_png(path: str, width: int, height: int, rows: list[bytes]) -> None:
    """8-bit greyscale PNG."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + row for row in rows)
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def cmd_screen(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    base = cartridge_window(elf)
    # display.h defines the offset as CHANDLER_FRAMEBUFFER_OFFSET (chandler.h).
    defs = include_defines()
    offset = defs["DISPLAY_BUFFER_OFFSET"]
    size = defs["DISPLAY_BUFFER_SIZE"]
    if size != FB_WIDTH * FB_HEIGHT // 8:
        raise SwdError(f"framebuffer is {size} bytes, expected 320x200 mono")
    fb = read_memory(base + offset, size)
    stride = FB_WIDTH // 8
    rows = []
    for y in range(FB_HEIGHT):
        line = fb[y * stride:(y + 1) * stride]
        # Bit 7 of each byte is the leftmost of its eight pixels. Lit pixels
        # are drawn black on white, as on the ST's monochrome screen.
        pixels = bytes(0 if (line[x >> 3] >> (7 - (x & 7))) & 1 else 255
                       for x in range(FB_WIDTH))
        scaled = bytes(p for p in pixels for _ in range(args.scale))
        rows.extend([scaled] * args.scale)
    write_png(args.out, FB_WIDTH * args.scale, FB_HEIGHT * args.scale, rows)
    lit = sum(bin(b).count("1") for b in fb)
    print(f"wrote {args.out} ({FB_WIDTH * args.scale}x{FB_HEIGHT * args.scale}, "
          f"{lit} pixels lit) from 0x{base + offset:08x}")
    return 0


WINDOW_BYTES = 128 * 1024
ST_ROM4 = 0xFA0000


def st_words(mem: bytes) -> bytes:
    """RP memory holds the ST's big-endian words byte-swapped; undo it."""
    out = bytearray(len(mem))
    out[0::2], out[1::2] = mem[1::2], mem[0::2]
    return bytes(out)


def rom_image(path: str) -> bytes:
    """A ROM file as the loader stores it: a STEEM image loses its 4-byte
    zero header (the loader's rule: 4 zero bytes, then whole flash sectors)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > 4 and (len(data) - 4) % 4096 == 0 and data[:4] == bytes(4):
        data = data[4:]
    return data


def cmd_window(args: argparse.Namespace) -> int:
    """Is the ROM the ST reads exactly this file? Checks the cartridge window
    (what the bus serves) and ROM_TEMP in flash (what a reboot copies in)."""
    elf = matching_elf(args.elf)
    image = rom_image(args.file)
    places = [("window", cartridge_window(elf))]
    if not args.no_flash:
        rom_temp = elf_symbols(elf, "_rom_temp_start").get("_rom_temp_start")
        if rom_temp:
            places.append(("ROM_TEMP", rom_temp[0]))
    failed = False
    for name, address in places:
        st = st_words(read_memory(address, WINDOW_BYTES))
        size = min(len(image), WINDOW_BYTES)
        diff = next((i for i in range(size) if st[i] != image[i]), None)
        if len(image) > WINDOW_BYTES:
            print(f"{name}: the file is {len(image)} bytes, more than the "
                  f"{WINDOW_BYTES}-byte window")
            failed = True
        if diff is None:
            print(f"{name}: PASS, {size} bytes match")
        else:
            bank = "ROM4" if diff < 0x10000 else "ROM3"
            print(f"{name}: FAIL at ${ST_ROM4 + diff:06X} ({bank}): "
                  f"reads 0x{st[diff]:02x}, the file has 0x{image[diff]:02x}")
            failed = True
        tail = st[size:]
        if tail:
            erased = tail.count(0xFF)
            zero = tail.count(0)
            other = len(tail) - erased - zero
            print(f"  after the ROM, {len(tail)} bytes: {erased} erased "
                  f"(0xff), {zero} zero, {other} other" +
                  (" (left over from an earlier ROM?)" if other else ""))
    return 3 if failed else 0


DMA_BASE = 0x50000000
DMA_CHANNELS = 8


def st_command_word(elf: str) -> int:
    """Where the RP leaves a command for the ST's menu loop: the template's
    sentinel, or before it the word after the framebuffer."""
    defs = include_defines()
    base = cartridge_window(elf)
    if "CHANDLER_CMD_SENTINEL_OFFSET" in defs:
        return base + defs["CHANDLER_CMD_SENTINEL_OFFSET"]
    return (base + defs["DISPLAY_BUFFER_OFFSET"] +
            defs["DISPLAY_COMMAND_ADDRESS_OFFSET"])


def write_st_command(address: int, value: int) -> None:
    """The ST reads the long as two big-endian words: store it word-swapped,
    as the firmware's WRITE_AND_SWAP_LONGWORD does."""
    swapped = ((value << 16) | (value >> 16)) & 0xFFFFFFFF
    openocd(f"mww 0x{address:08x} 0x{swapped:08x}")


def st_reads(elf: str, samples: int = 8) -> list[int]:
    """The last cartridge addresses the ST read, as offsets into the window:
    the DMA channel that serves the bus keeps its read address at the word it
    served last."""
    base = cartridge_window(elf)
    cmds = []
    for _ in range(samples):
        cmds += [f"mdw 0x{DMA_BASE + 0x40 * ch:08x}" for ch in range(DMA_CHANNELS)]
        cmds.append("sleep 20")
    out = openocd(*cmds, check=False)
    values = [int(v, 16) for v in re.findall(r"0x500000[0-9a-f]{2}: ([0-9a-f]+)", out)]
    return [v - base for v in values if base <= v < base + WINDOW_BYTES]


# The remote reset agent the self-check cartridge leaves in the ST's RAM
# (tools/dev/selfcheck/agent.s): its signature, AGENT_SIG_ADDR - $FA0000.
AGENT_SIG_OFFSET = 0x9F44
AGENT_SIG = (0x52535421, 0x4147454E)    # "RST!", "AGEN"


def cmd_st_reset(args: argparse.Namespace) -> int:
    """Reboot the ST through whichever of the two things on it that watch the
    cartridge window is running: the setup menu's loop, which polls its
    command word every frame, or the remote reset agent the self-check
    cartridge leaves in RAM, which looks for its signature every vertical
    blank (in GEM, or in a ROM that keeps TOS's interrupts). Both are written,
    then every word goes back to what it was, so the rebooted ST does not
    reset again and a ROM in the window stays whole. A ROM that took the ST
    over (a game, DiagROM) still needs the reset button."""
    elf = matching_elf(args.elf)
    defs = include_defines()
    reset = defs.get("CHANDLER_CMD_RESET", defs.get("DISPLAY_COMMAND_RESET", 1))
    # --offset: the ST runs another firmware's menu loop (the one flashed
    # before this ELF), which polls its own word: the template's sentinel is
    # at 0x2000 of the window, v2.1.2's command word at 0x9f40.
    base = cartridge_window(elf)
    address = base + int(args.offset, 0) if args.offset else st_command_word(elf)
    agent = base + AGENT_SIG_OFFSET
    # Who is listening: the agent reads its signature every vertical blank,
    # the menu reads the framebuffer every frame; the bus DMA keeps the last
    # address the ST read.
    fb = defs.get("CHANDLER_FRAMEBUFFER_OFFSET", defs.get("DISPLAY_BUFFER_OFFSET"))
    seen = st_reads(elf)
    if any(AGENT_SIG_OFFSET <= o < AGENT_SIG_OFFSET + 8 for o in seen):
        print("the remote reset agent is watching")
    elif fb is not None and sum(fb <= o < 0x10000 for o in seen) >= 2:
        print("the setup menu is watching")
    else:
        print("nothing on the ST is reading the cartridge: if this does not "
              "reboot it, it needs its reset button", file=sys.stderr)
    writes = [(address, reset), (agent, AGENT_SIG[0]), (agent + 4, AGENT_SIG[1])]
    saved = {a: read_word(a) for a, _ in writes}
    swap = lambda v: ((v << 16) | (v >> 16)) & 0xFFFFFFFF   # as write_st_command
    openocd(*[f"mww 0x{a:08x} 0x{swap(v):08x}" for a, v in writes])
    time.sleep(0.6)
    openocd(*[f"mww 0x{a:08x} 0x{saved[a]:08x}" for a, _ in reversed(writes)])
    print(f"reset command written at 0x{address:08x} and the agent's signature at "
          f"0x{agent:08x}, then the words as they were")
    if not args.wait:
        return 0
    fb = defs.get("CHANDLER_FRAMEBUFFER_OFFSET", defs.get("DISPLAY_BUFFER_OFFSET"))
    booted = False
    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline:
        offsets = st_reads(elf)
        if any(o < 0x100 for o in offsets):
            booted = True          # the cartridge header: TOS looked for it
            if args.rom:
                print("the ST booted through the cartridge")
                return 0
        if booted and sum(fb <= o < 0x10000 for o in offsets) >= 2:
            print("the ST booted through the cartridge and is back in the "
                  "menu loop")
            return 0
    print("the ST " + ("booted but did not reach the menu loop"
                       if booted else "did not boot through the cartridge") +
          f" within {args.wait:g} s", file=sys.stderr)
    return 3


TIMER_DBGPAUSE = 0x4005402C


def cmd_debug_pause(args: argparse.Namespace) -> int:
    """The RP2040's timer stops while a core is halted in debug (DBGPAUSE,
    0x7 after every reset). With the probe attached, a core that runs into a
    breakpoint instruction (fatfs-sdk's crash handler ends in one) halts, the
    timer stops, and the other core freezes in its next sleep_ms, which is not
    what happens without a probe. `off` keeps the timer running, as in the
    field, until the next reset."""
    value = 0x7 if args.state == "on" else 0x0
    openocd(f"mww 0x{TIMER_DBGPAUSE:08x} 0x{value:x}")
    print(f"timer debug pause {args.state} (DBGPAUSE 0x{value:x}) until the next reset")
    return 0


RAM_BASE = 0x20000000


def line_addresses(elf: str, location: str) -> list[int]:
    """Where FILE:LINE's code starts, in program order: one address per place
    the line was compiled into (an inlined function's line has several)."""
    path, line = location.rsplit(":", 1)
    suffix = "/" + path.lstrip("/")
    marker = re.compile(r"^(/\S+):(\d+)(?: \(discriminator \d+\))?$")
    insn = re.compile(r"^\s*([0-9a-f]+):\t")
    out = subprocess.run(["arm-none-eabi-objdump", "-dl", elf],
                         capture_output=True, text=True, check=True).stdout
    found, wanted = [], False
    for text in out.splitlines():
        m = marker.match(text)
        if m:
            wanted = m.group(1).endswith(suffix) and int(m.group(2)) == int(line)
            continue
        m = insn.match(text)
        if m and wanted:
            found.append(int(m.group(1), 16))
            wanted = False
    return found


def parse_stop(elf: str, text: str) -> tuple[str, int, list[tuple[str, int]]]:
    """A boot-break stop, "LOCATION[@REG=VALUE,...]": its label, its address
    and the registers to set there."""
    location, _, sets_text = text.partition("@")
    if location.lower().startswith("0x"):
        address = int(location, 16)
    else:
        found = line_addresses(elf, location)
        if not found:
            raise SwdError(f"no code for {location} in {elf}")
        address = found[0]
        if len(found) > 1:
            print(f"{location} is at " + ", ".join(f"0x{a:08x}" for a in found)
                  + "; stopping at the first", flush=True)
    sets = []
    for item in filter(None, sets_text.split(",")):
        reg, _, value = item.partition("=")
        sets.append((reg, int(value, 0)))
    return location, address, sets


def cmd_boot_break(args: argparse.Namespace) -> int:
    """Restart the RP and stop core 0 at each STOP on the way up, in order,
    setting registers there, then let it run on: a boot path taken
    differently without a rebuild (an f_open made to report no file, a mode
    read as another).

    The restart is OpenOCD's `reset halt`, which holds both cores at the boot
    ROM's first instruction: a hardware breakpoint set before the
    watchdog-style reset does not survive it. The RP2040's hardware
    breakpoints only cover flash, so for code that runs from RAM (copied
    there at boot) core 0 first stops at --via, a flash function reached after
    the copy, and software breakpoints go in then. One OpenOCD runs the whole
    boot through its TCL port: each stop's breakpoint goes in only after the
    one before it was hit, its registers are written only once the PC is
    checked, and every breakpoint comes out again, so a stop that is never
    reached changes nothing."""
    elf = matching_elf(args.elf)
    stops = [parse_stop(elf, s) for s in args.stops]
    if any(address >= RAM_BASE for _, address, _ in stops):
        via = elf_symbols(elf, args.via).get(args.via)
        if not via:
            raise SwdError(f"no {args.via} in {elf}")
        stops.insert(0, (args.via, via[0] & ~1, []))
    server = openocd_command() + [
        "-c", f"tcl_port {TCL_PORT}", "-c", "gdb_port disabled",
        "-c", "telnet_port disabled"]
    proc = subprocess.Popen(server, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    reached = 0
    try:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line or f"port {TCL_PORT}" in line:
                break
        threading.Thread(target=proc.stdout.read, daemon=True).start()
        sock = tcl_server()
        if sock is None:
            raise SwdError("OpenOCD did not start its TCL server")
        with sock:
            def run(command: str) -> str:
                return tcl(sock, "capture {" + command + "}")

            def pc() -> int | None:
                m = re.search(r"pc \(/32\): (0x[0-9a-f]+)", run("reg pc"))
                return int(m.group(1), 16) if m else None

            run(f"targets {CORES[0]}")
            run("reset halt")
            for label, address, sets in stops:
                kind = " hw" if address < RAM_BASE else ""
                run(f"bp 0x{address:08x} 2{kind}")
                run("resume")
                run(f"wait_halt {int(args.timeout * 1000)}")
                if pc() is None:          # still running: stop it to clean up
                    run("halt")
                    run("wait_halt 2000")
                here = pc()
                run(f"rbp 0x{address:08x}")
                if here != address:
                    print(f"core 0 did not stop at {label} (0x{address:08x}) "
                          f"within {args.timeout:g} s; nothing set there",
                          file=sys.stderr)
                    break
                before = ", ".join(
                    f"{r}=" + re.search(r": (0x[0-9a-f]+)", run(f"reg {r}")).group(1)
                    for r, _ in sets)
                for r, v in sets:
                    run(f"reg {r} 0x{v:x}")
                reached += 1
                print(f"stopped at {label} (0x{address:08x})"
                      + (f": {before} -> " + ", ".join(f"{r}=0x{v:x}" for r, v in sets)
                         if sets else ""), flush=True)
            run("resume")
            if args.park_core1:
                # Core 1 runs from flash: halt it before core 0 writes flash
                # (v2.1.2's settings-save lock-up), with the timer running.
                run(f"mww 0x{TIMER_DBGPAUSE:08x} 0x0")
                run(f"targets {CORES[1]}")
                run(f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE | 0x3:08x}")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    released = CORES[:1] if args.park_core1 else CORES
    if any(read_word(DHCSR, core) & DHCSR_S_HALT for core in released):
        cmd_resume(args)
        if args.park_core1:
            openocd(f"targets {CORES[1]}",
                    f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE | 0x3:08x}")
    if reached < len(stops):
        return 3
    print("running on")
    return 0


SCRATCH_X = 0x20040000          # core 1's stack bank
SCRATCH_Y = 0x20041000          # core 0's stack bank
SCRATCH_END = 0x20042000
STACK_PAINT = 0x5AFE57AC        # a word no stack frame is likely to hold
STACK_MARGIN = 64               # left untouched below each live stack pointer


def cmd_stack(args: argparse.Namespace) -> int:
    """Paint the free part of both stack banks (SCRATCH_X, SCRATCH_Y) with a
    known word, or report how deep each stack has gone since. Painting halts
    both cores for a moment, so no interrupt frame lands on a word being
    painted; the bus keeps being served by the DMA meanwhile. SRAM survives
    a watchdog reset, so a paint also measures the next boot."""
    if args.action == "paint":
        try:
            # Halt both cores, then read each one's main stack pointer
            # through its own debug registers (DCRSR register 17 is MSP):
            # OpenOCD's `reg sp` does not answer for core 1 here.
            cmds = ["halt"]
            for core in CORES:
                cmds += [f"targets {core}", "mww 0xe000edf4 17", "sleep 1",
                         "mdw 0xe000edf8"]
            out = openocd(*cmds, check=False)
            sps = [int(v, 16) for v in re.findall(r"0xe000edf8: ([0-9a-f]+)", out)]
            if len(sps) < 2:
                raise SwdError("could not read both stack pointers")
            sp0, sp1 = sps[0], sps[1]
            # Core 1 runs only if its stack pointer is inside its own bank.
            if SCRATCH_X <= sp1 < SCRATCH_Y:
                regions = [(SCRATCH_X, sp1 - STACK_MARGIN),
                           (SCRATCH_Y, sp0 - STACK_MARGIN)]
            else:
                regions = [(SCRATCH_X, sp0 - STACK_MARGIN)]
            fills = [f"mww 0x{lo & ~3:08x} 0x{STACK_PAINT:08x} "
                     f"{((hi & ~3) - (lo & ~3)) // 4}"
                     for lo, hi in regions if hi > lo]
            openocd(*fills)
        finally:
            # A fresh session releases both cores (C_HALT and C_DEBUGEN):
            # in the session that halted them, the release does not take.
            cmd_resume(args)
        for lo, hi in regions:
            print(f"painted 0x{lo:08x}-0x{hi:08x}")
        print(f"core 0 sp 0x{sp0:08x}, core 1 sp 0x{sp1:08x}")
        return 0
    data = read_memory(SCRATCH_X, SCRATCH_END - SCRATCH_X)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    for name, lo, top in (("core 1's bank (SCRATCH_X)", SCRATCH_X, SCRATCH_Y),
                          ("core 0's bank (SCRATCH_Y)", SCRATCH_Y, SCRATCH_END)):
        first = (lo - SCRATCH_X) // 4
        last = (top - SCRATCH_X) // 4
        painted = [i for i in range(first, last) if words[i] == STACK_PAINT]
        if not painted:
            print(f"{name}: no paint left")
            continue
        # The lowest word still painted from the bottom up: everything above
        # the first unpainted word was reached by a stack.
        low = first
        while low < last and words[low] == STACK_PAINT:
            low += 1
        reached = SCRATCH_X + 4 * low
        print(f"{name}: untouched up to 0x{reached:08x}, "
              f"{top - reached} bytes reached from the top 0x{top:08x}")
    return 0


def cmd_text(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    sym = elf_symbols(elf, "screen").get("screen")
    if not sym or not sym[1]:
        raise SwdError(f"{os.path.basename(elf)} has no term screen buffer")
    defs = include_defines()
    width = defs.get("TERM_SCREEN_SIZE_X", 40)
    data = read_memory(sym[0], sym[1])
    for y in range(sym[1] // width):
        row = data[y * width:(y + 1) * width]
        print("".join(chr(c) if 32 <= c < 127 else " " for c in row).rstrip())
    return 0


def svar_names() -> dict[int, list[str]]:
    """Slot index -> names, from the *_SVAR_* / *_SHARED_VARIABLE_* indexes
    that all drivers define into the one shared-variables array."""
    names: dict[int, list[str]] = {}
    for name, value in include_defines().items():
        if name.endswith(("_SIZE", "_OFFSET")):
            continue
        if not ("_SVAR_" in name or "SHARED_VARIABLE_" in name or
                name in ("CHANDLER_HARDWARE_TYPE", "CHANDLER_SVERSION",
                         "CHANDLER_BUFFER_TYPE")):
            continue
        if 0 <= value < SVAR_MAX_SLOTS:
            names.setdefault(value, []).append(name)
    return names


def cmd_shared(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    base = cartridge_window(elf)
    defs = header_defines(os.path.join(INCLUDE_DIR, "chandler.h"))
    start = defs["CHANDLER_CMD_SENTINEL_OFFSET"]
    var_off = defs["CHANDLER_SHARED_VARIABLES_OFFSET"]
    names = svar_names()
    slots = min(SVAR_MAX_SLOTS, (max(names) if names else 0) + 1)
    data = read_memory(base + start, var_off - start + slots * 4)

    def long_at(offset: int) -> int:
        # The ST reads big-endian longs made of two RP-order 16-bit words.
        hi, lo = struct.unpack_from("<HH", data, offset - start)
        return (hi << 16) | lo

    print(f"cartridge window 0x{base:08x} (ST $FA0000), ELF {os.path.basename(elf)}")
    for label, name in (("command sentinel", "CHANDLER_CMD_SENTINEL_OFFSET"),
                        ("random token", "CHANDLER_RANDOM_TOKEN_OFFSET"),
                        ("random token seed",
                         "CHANDLER_RANDOM_TOKEN_SEED_OFFSET")):
        off = defs[name]
        print(f"  ${0xFA0000 + off:06X}  {label:<34} 0x{long_at(off):08x}")
    for i in range(slots):
        value = long_at(var_off + i * 4)
        label = " / ".join(names.get(i, []))
        # The gaps between the drivers' blocks hold their data (BPBs, buffers),
        # so unnamed slots are noise unless asked for.
        if not label and (value == 0 or not args.all):
            continue
        print(f"  ${0xFA0000 + var_off + i * 4:06X}  [{i:4}] {label or '-':<29} "
              f"0x{value:08x}  {value}")
    return 0


def cmd_read(args: argparse.Namespace) -> int:
    data = read_memory(int(args.address, 0), int(args.length, 0))
    with open(args.outfile, "wb") as f:
        f.write(data)
    print(f"read {len(data)} bytes from {args.address} into {args.outfile}")
    return 0


HEAP_SYMBOLS = ("end", "__StackLimit", "heap_end.0", "__malloc_sbrk_base",
                "__malloc_max_sbrked_mem", "__malloc_av_")


def heap_snapshot(elf: str) -> dict:
    """Heap figures read from newlib's own malloc state, no firmware help.

    The heap grows by sbrk from `end` towards `__StackLimit` and never gives
    memory back, so __malloc_max_sbrked_mem is the peak the heap ever reached,
    transient peaks included. Free space inside the heap comes from walking
    its chunks: each chunk's size word carries PREV_INUSE for the chunk
    before it, and the walk ends at the top chunk (av_[2]). The walk reads
    RAM while the CPU runs, so a chunk that changes mid-read can break it:
    `walk_ok` is False then and the caller may simply read again."""
    sym = elf_symbols(elf, *HEAP_SYMBOLS)
    missing = [n for n in HEAP_SYMBOLS if n not in sym]
    if missing:
        raise SwdError(f"{os.path.basename(elf)} lacks {', '.join(missing)}")
    start, limit = sym["end"][0], sym["__StackLimit"][0]
    brk, base, peak, top = struct.unpack("<4I", b"".join(
        read_memory(sym[n][0] + off, 4) for n, off in
        (("heap_end.0", 0), ("__malloc_sbrk_base", 0),
         ("__malloc_max_sbrked_mem", 0), ("__malloc_av_", 8))))
    snap = {"size": limit - start, "arena": (brk - start) if brk else 0,
            "peak": peak, "headroom": limit - (brk or start),
            "peak_headroom": limit - start - peak, "walk_ok": False}
    if not brk or base in (0, 0xFFFFFFFF) or not (base <= top < brk):
        return snap
    heap = read_memory(base, brk - base)

    def word(addr: int) -> int:
        return struct.unpack_from("<I", heap, addr - base)[0]

    used = free = largest = free_chunks = 0
    # The first chunk is not always at the sbrk base: malloc moves it forward
    # so that its user pointer (chunk + 8) is 8-byte aligned.
    p = base + (-(base + 8) & 7)
    while p < top:
        size = word(p + 4) & ~3
        if size < 16 or p + size > top:
            return snap
        if word(p + size + 4) & 1:
            used += size
        else:
            free += size
            free_chunks += 1
            largest = max(largest, size)
        p += size
    top_size = word(top + 4) & ~3
    if p != top or top + top_size > brk + 16:
        return snap
    snap.update(walk_ok=True, used=used, free=free + top_size,
                largest=max(largest, top_size), free_chunks=free_chunks)
    return snap


def heap_line(snap: dict) -> str:
    kb = lambda n: f"{n / 1024:.1f} KB"
    line = (f"heap {kb(snap['size'])}: arena {kb(snap['arena'])}, "
            f"peak {kb(snap['peak'])} (closest to the stack: "
            f"{kb(snap['peak_headroom'])}), never used {kb(snap['headroom'])}")
    if snap["walk_ok"]:
        line += (f"; in use {kb(snap['used'])}, free in arena "
                 f"{kb(snap['free'])} in {snap['free_chunks'] + 1} blocks, "
                 f"largest {kb(snap['largest'])}")
    else:
        line += "; chunk walk failed (heap changed while reading)"
    return line


def cmd_counters(args: argparse.Namespace) -> int:
    """The command channel's counters, read while the RP runs."""
    elf = matching_elf(args.elf)
    sym = elf_symbols(elf, *COUNTERS, *OPTIONAL_COUNTERS)
    missing = [n for n in COUNTERS if n not in sym]
    if missing:
        raise SwdError(f"{os.path.basename(elf)} lacks {', '.join(missing)}")
    names = [n for n in COUNTERS + OPTIONAL_COUNTERS if n in sym]
    first, last = min(sym[n][0] for n in names), max(sym[n][0] for n in names)
    data = read_memory(first, last - first + 4)
    v = {n: struct.unpack_from("<I", data, sym[n][0] - first)[0] for n in names}
    n = v["chandlerHandled"]
    avg = lambda key: f"{v[key] / n:.0f}" if n else "-"
    print(f"commands answered {n}, dropped while one was pending "
          f"{v['chandlerDropped']}, repeated tokens {v['chandlerRepeated']}, "
          f"checksum errors {v['chandlerChecksumErrors']}, ring overruns "
          f"{v['commOverruns']}")
    print(f"per command: busy {avg('chandlerBusyUs')} us (max "
          f"{v['chandlerMaxBusyUs']}), gap to the next {avg('chandlerGapUs')} us "
          f"(max {v['chandlerMaxGapUs']}), of which quiet {avg('chandlerQuietUs')} us "
          f"(max {v['chandlerMaxQuietUs']}); passes per frame "
          f"{v['chandlerFramePolls'] / n:.2f}" if n else "per command: -")
    injected = (f"; commands injected by the probe {v['chandlerInjected']}"
                if "chandlerInjected" in v else "")
    print(f"ring drained for {v['chandlerPollUs'] / 1e6:.3f} s in all{injected}")
    return 0


def command_names(defs: dict[str, int]) -> dict[int, str]:
    """Protocol command IDs to names: APP_<APP>_<COMMAND> defines under their
    APP_<APP> app ID, as term.h does, and chandler's framework commands."""
    names: dict[int, str] = {}
    small = {k: v for k, v in defs.items() if k.startswith("APP_") and 0 <= v <= 0xFF}
    for name, value in sorted(small.items()):
        parts = name.split("_")
        for cut in range(len(parts) - 1, 1, -1):
            app = "_".join(parts[:cut])
            if app in small:
                names.setdefault((small[app] << 8) | value, name)
                break
    framework = defs.get("CHANDLER_APP_FRAMEWORK")
    for name, value in sorted(defs.items()):
        if (name.startswith("CHANDLER_") and framework is not None and
                0 <= value <= 0xFFFF and value >> 8 == framework):
            names.setdefault(value, name)
    return names


def ring_snapshot(elf: str) -> tuple[list[int], int, int]:
    """The capture ring's samples, oldest first, with the DMA's count of
    samples written when the read started and how many arrived during it.
    Only the samples the DMA cannot have overwritten while they were read are
    returned: the window ends at the write position read before the copy and
    loses one sample at its old end for each that arrived during it."""
    sym = elf_symbols(elf, "commRing", "commDmaChannel")
    if len(sym) < 2:
        raise SwdError(f"{os.path.basename(elf)} lacks commRing or commDmaChannel")
    base, size = sym["commRing"]
    words = size // 2
    channel = read_word(sym["commDmaChannel"][0])
    if channel >= DMA_CHANNELS:
        raise SwdError("the capture DMA channel is not claimed: "
                       "has commemul_init() run?")
    regs = DMA_BASE + DMA_CHANNEL_STRIDE * channel + DMA_WRITE_ADDR
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "ring.bin")
        out = openocd(f"mdw 0x{regs:08x} 2", f"dump_image {path} 0x{base:08x} {size}",
                      f"mdw 0x{regs:08x} 2")
        with open(path, "rb") as f:
            data = f.read()
    reads = re.findall(rf"0x{regs:08x}:\s+([0-9a-fA-F]{{8}})\s+([0-9a-fA-F]{{8}})", out)
    if len(reads) != 2 or len(data) != size:
        raise SwdError("cannot read the capture ring")
    (write1, left1), (_, left2) = [(int(a, 16), int(b, 16)) for a, b in reads]
    if not base <= write1 < base + size:
        raise SwdError(f"the capture DMA writes 0x{write1:08x}, outside the ring")
    total = header_defines(COMMEMUL_C)["COMM_DMA_TRANSFER_COUNT"]
    during = (left1 - left2) & 0xFFFFFFFF
    if during >= words:
        raise SwdError("the ST filled the ring while it was read: read again")
    samples = struct.unpack(f"<{words}H", data)
    end = (write1 - base) // 2
    written = (total - left1) & 0xFFFFFFFF
    # RAM survives a reset: past what this boot wrote lie the last boot's samples.
    keep = min(words - during, written)
    window = [samples[(end - keep + i) % words] for i in range(keep)]
    return window, written, during


def decode_frames(values: list[int], header: int,
                  max_payload: int) -> list[dict]:
    """Frames in a stream of 16-bit values, parsed as tprotocol_parse() does:
    header, command, payload size in bytes, the payload's words, and a checksum
    of the command, the size and the payload."""
    frames = []
    i, n = 0, len(values)
    while i < n:
        if values[i] != header:
            i += 1
            continue
        if i + 2 >= n:
            frames.append({"pos": i, "incomplete": True})
            break
        command, size = values[i + 1], values[i + 2]
        if size > max_payload:
            frames.append({"pos": i, "command": command, "size": size,
                           "oversize": True})
            i += 3  # the parser goes back to looking for a header
            continue
        last = i + 3 + (size + 1) // 2
        if last >= n:
            frames.append({"pos": i, "incomplete": True, "command": command})
            break
        payload = values[i + 3:last]
        expected = (command + size + sum(payload[:size // 2])) & 0xFFFF
        frames.append({"pos": i, "command": command, "size": size,
                       "payload": payload, "ok": values[last] == expected})
        i = last + 1
    return frames


def cmd_ring(args: argparse.Namespace) -> int:
    """The commands the ST sent, decoded from the ROM3 capture ring."""
    elf = matching_elf(args.elf)
    defs = include_defines()
    samples, written, during = ring_snapshot(elf)
    if args.mark:
        os.makedirs(os.path.dirname(RING_MARK), exist_ok=True)
        with open(RING_MARK, "w", encoding="utf-8") as f:
            f.write(f"{written}\n")
        print(f"marked at sample {written}")
        return 0
    first = written - len(samples)
    note = ""
    if args.since_mark:
        try:
            with open(RING_MARK, encoding="utf-8") as f:
                mark = int(f.read())
        except (OSError, ValueError):
            raise SwdError(f"no mark: run `ring --mark` first ({RING_MARK})")
        if mark > written:
            note = "the RP restarted since the mark: the whole ring follows"
        elif mark < first:
            note = (f"{first - mark} samples since the mark were overwritten "
                    "before this read")
        else:
            samples = samples[mark - first:]
            first = mark
    flip = defs["CHANDLER_ADDRESS_HIGH_BIT"]
    values = [s ^ flip for s in samples]
    frames = decode_frames(values, defs["PROTOCOL_HEADER"],
                           defs["MAX_PROTOCOL_PAYLOAD_SIZE"])
    names = command_names(defs)
    for fr in frames:
        at = f"#{first + fr['pos']}"
        if fr.get("incomplete"):
            print(f"{at:>12}  (a frame still arriving)")
            continue
        name = names.get(fr["command"], f"0x{fr['command']:04x}")
        line = f"{at:>12}  {name:28}{fr['size']:5} B"
        if fr.get("oversize"):
            print(f"{line}  past MAX_PROTOCOL_PAYLOAD_SIZE: dropped, as the RP does")
            continue
        p = fr["payload"]
        if len(p) >= 2:
            line += f"  token 0x{(p[0] << 16) | p[1]:08x}"
        params = [(p[k + 1] << 16) | p[k] for k in range(2, len(p) - 1, 2)]
        line += "".join(f" 0x{x:08x}" for x in params[:RING_PARAMS])
        if len(params) > RING_PARAMS:
            line += " ..."
        if not fr["ok"]:
            line += "  CHECKSUM MISMATCH"
        print(line)
    whole = [fr for fr in frames if "ok" in fr]
    bad = sum(1 for fr in whole if not fr["ok"])
    dropped = sum(1 for fr in frames if fr.get("oversize"))
    span = f"#{first} to #{written - 1}" if samples else "none yet"
    print(f"{len(whole)} frames, {bad} with a bad checksum"
          + (f", {dropped} oversize dropped" if dropped else "")
          + f", in {len(samples)} samples ({span}); {during} arrived while reading")
    if note:
        print(note)
    return 0


def cmd_heap(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    csv = None
    if args.csv:
        new = not os.path.exists(args.csv)
        csv = open(args.csv, "a")
        if new:
            csv.write("time,arena,peak,peak_headroom,headroom,used,free,"
                      "largest,free_blocks\n")
    try:
        while True:
            snap = heap_snapshot(elf)
            if not snap["walk_ok"]:
                snap = heap_snapshot(elf)
            print(time.strftime("%H:%M:%S ") + heap_line(snap), flush=True)
            if csv:
                csv.write(",".join([time.strftime("%Y-%m-%d %H:%M:%S")] + [
                    str(snap.get(k, "")) for k in
                    ("arena", "peak", "peak_headroom", "headroom", "used",
                     "free", "largest")] +
                    [str(snap["free_chunks"] + 1) if snap["walk_ok"] else ""])
                    + "\n")
                csv.flush()
            if not args.watch:
                return 0
            time.sleep(args.watch)
    except KeyboardInterrupt:
        return 0
    finally:
        if csv:
            csv.close()


def cmd_resume(args: argparse.Namespace) -> int:
    """Release both cores from a debug halt. OpenOCD's own resume fails in a
    new OpenOCD run, and a halted core 1 also pauses the RP2040's timer,
    which leaves core 0 asleep forever."""
    commands = []
    for core in CORES:
        commands += [f"targets {core}", f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
    openocd(*commands)
    states = [read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES]
    if any(states):
        print("still halted: " + ", ".join(
            c for c, h in zip(CORES, states) if h), file=sys.stderr)
        return 3
    print("both cores released")
    return 0


def include_defines() -> dict[str, int]:
    """Every integer #define in rp/src/include, resolved across headers."""
    return header_defines(*sorted(glob.glob(os.path.join(INCLUDE_DIR, "*.h"))))


def cmd_select(args: argparse.Namespace) -> int:
    defs = include_defines()
    gpio = defs["SELECT_GPIO"]
    ctrl = IO_BANK0 + 4 + 8 * gpio
    normal = read_word(ctrl) & ~(3 << INOVER_SHIFT)
    if args.press == "release":
        openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
        print(f"SELECT (GPIO {gpio}) override cleared")
        return 0
    if args.press == "long" and not args.force:
        raise SwdError("a long press is a factory reset: it erases the global "
                       "settings, and Booster then clears every app's "
                       "settings (reset_deviceAndEraseFlash): add --force")
    hold = args.hold_ms or (SELECT_SHORT_MS if args.press == "short"
                            else defs["SELECT_LONG_RESET"] + 1000)
    pressed = normal | (INOVER_HIGH << INOVER_SHIFT)
    try:
        openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}", f"sleep {hold}",
                f"mww 0x{ctrl:08x} 0x{normal:08x}")
    finally:
        # Never leave the button pressed, even if OpenOCD failed mid-hold.
        if read_word(ctrl) & (3 << INOVER_SHIFT):
            openocd(f"mww 0x{ctrl:08x} 0x{normal:08x}")
    print(f"SELECT (GPIO {gpio}) held {hold} ms")
    return 0


def mailbox_request(elf: str, kind: int, command_id: int, words: list[int],
                    timeout: float = 5.0) -> int:
    """Send one request through the debug mailbox; return its result."""
    if len(words) > MAILBOX_WORDS:
        raise SwdError(f"at most {MAILBOX_WORDS} payload words")
    sym = elf_symbols(elf, MAILBOX_SYMBOL).get(MAILBOX_SYMBOL)
    if not sym:
        raise SwdError(f"{os.path.basename(elf)} has no {MAILBOX_SYMBOL}: "
                       "a debug build is needed")
    base = sym[0]
    magic, seq, ack = struct.unpack("<III", read_memory(base, 12))
    if magic != MAILBOX_MAGIC:
        raise SwdError(f"no mailbox at 0x{base:08x} (magic 0x{magic:08x})")
    if seq != ack:
        raise SwdError("the previous request was never acknowledged: "
                       "is the main loop running?")
    commands = [f"mww 0x{base + MB_KIND:08x} {kind}",
                f"mwh 0x{base + MB_CMD:08x} {command_id}",
                f"mwh 0x{base + MB_SIZE:08x} {len(words) * 2}"]
    for i, word in enumerate(words):
        commands.append(f"mwh 0x{base + MB_PAYLOAD + 2 * i:08x} {word & 0xFFFF}")
    # seq last: the firmware acts as soon as seq differs from ack.
    commands.append(f"mww 0x{base + MB_SEQ:08x} {ack + 1}")
    openocd(*commands)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new_ack, _, result = struct.unpack(
            "<III", read_memory(base + MB_ACK, MB_RESULT + 4 - MB_ACK))
        if new_ack == ack + 1:
            return result
        time.sleep(0.02)
    raise SwdError(f"no acknowledge within {timeout:g} s: "
                   "is the main loop running?")


def send_protocol_slot(elf: str, command_id: int, words: list[int],
                       timeout: float = 5.0) -> bool:
    """Firmware without the mailbox: write the frame where its ROM3 interrupt
    leaves a parsed command (lastProtocol, then lastProtocolValid) and wait
    for the main loop to take it. False when the ELF has no such slot."""
    sym = elf_symbols(elf, "lastProtocol", "lastProtocolValid")
    if len(sym) < 2:
        return False
    slot, flag = sym["lastProtocol"][0], sym["lastProtocolValid"][0]

    def pending() -> bool:
        return read_memory(flag, 1)[0] != 0

    deadline = time.monotonic() + timeout
    while pending():
        if time.monotonic() > deadline:
            raise SwdError("the firmware kept another command pending")
        time.sleep(0.1)
    # TransmissionProtocol: command_id, payload_size, bytes_read,
    # final_checksum (16 bits each), then the payload. The flag goes last:
    # the main loop acts as soon as it is set, and clears it when done.
    commands = [f"mwh 0x{slot:08x} {command_id}",
                f"mwh 0x{slot + 2:08x} {len(words) * 2}"]
    for i, word in enumerate(words):
        commands.append(f"mwh 0x{slot + 8 + 2 * i:08x} {word & 0xFFFF}")
    commands.append(f"mwb 0x{flag:08x} 1")
    openocd(*commands)
    while pending():
        if time.monotonic() > deadline:
            raise SwdError(f"not taken within {timeout:g} s: "
                           "is the main loop running?")
        time.sleep(0.1)
    return True


def send_protocol(elf: str, command_id: int, words: list[int]) -> None:
    # The payload starts with the random token, which only matters to the ST.
    if (MAILBOX_SYMBOL not in elf_symbols(elf, MAILBOX_SYMBOL)
            and send_protocol_slot(elf, command_id, [0, 0] + words)):
        return
    for _ in range(10):
        if mailbox_request(elf, KIND_PROTOCOL, command_id, [0, 0] + words):
            return
        time.sleep(0.1)
    raise SwdError("the firmware kept another command pending")


# Keys by name, as the ST's Bconin reports them: (ASCII, scan code). The
# cursor keys come with ASCII 0.
NAMED_KEYS = {"up": (0, 72), "down": (0, 80), "left": (0, 75),
              "right": (0, 77), "esc": (27, 1), "return": (13, 28),
              "space": (32, 57)}


def send_key(elf: str, defs: dict[str, int], char: str, shift: bool = False,
             scan: int = 0) -> None:
    if char in NAMED_KEYS:
        ascii_code, scan = NAMED_KEYS[char]
    elif len(char) == 1:
        ascii_code = ord(char)
    else:
        raise SwdError("CHAR must be one character or one of "
                       + ", ".join(NAMED_KEYS))
    command_id = (defs["APP_TERMINAL"] << 8) | defs["APP_TERMINAL_KEYSTROKE"]
    param = (ascii_code | (scan << defs["TERM_KEYBOARD_SCAN_SHIFT"]) |
             ((1 if shift else 0) << defs["TERM_KEYBOARD_SHIFT_SHIFT"]))
    send_protocol(elf, command_id, [param & 0xFFFF, param >> 16])


def cmd_key(args: argparse.Namespace) -> int:
    send_key(matching_elf(args.elf), include_defines(), args.char, args.shift,
             args.scan)
    print(f"key {args.char!r} sent")
    return 0


def cmd_type(args: argparse.Namespace) -> int:
    """Type a line, a key at a time, through one OpenOCD: each `swd.py key`
    starts its own, about a second per key."""
    elf = matching_elf(args.elf)
    defs = include_defines()
    keys = ["space" if c == " " else c for c in args.text]
    keys += ["return"] * args.enter
    proc = None
    if tcl_server() is None:
        proc = subprocess.Popen(
            openocd_command() + ["-c", f"tcl_port {TCL_PORT}",
                                 "-c", "gdb_port disabled",
                                 "-c", "telnet_port disabled"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 10
        while tcl_server() is None:
            if proc.poll() is not None or time.monotonic() > deadline:
                raise SwdError("openocd did not start its TCL server")
            time.sleep(0.1)
    try:
        for key in keys:
            send_key(elf, defs, key)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
    print(f"typed {len(keys)} keys")
    return 0


def cmd_inject(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    send_protocol(elf, int(args.command_id, 0),
                  [int(w, 0) for w in args.words])
    print(f"command {args.command_id} injected")
    return 0


def cmd_app(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    name = "DEVHOOKS_APP_" + args.name.upper()
    command_id = include_defines().get(name)
    if command_id is None:
        raise SwdError(f"no {name} define in rp/src/include")
    words = [int(w, 0) for w in args.words]
    result = mailbox_request(elf, KIND_APP, command_id, words)
    print(f"{name}: result {result}")
    return 0 if result else 3


def resolve_address(elf: str, address: int) -> str:
    """`function at file:line` for a code address, or '' when not code."""
    if not (0x10000000 <= address < 0x10100000 or
            0x20000000 <= address < 0x20030000):
        return ""
    out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-i", "-p", "-e",
                          elf, f"0x{address & ~1:08x}"],
                         capture_output=True, text=True).stdout.strip()
    return "" if out.startswith("??") else out.replace("\n", "; ")


def header_enums(path: str) -> dict[str, dict[int, str]]:
    """Values of each `typedef enum {...} name;` in a C header."""
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    enums = {}
    for body, name in re.findall(r"typedef\s+enum\s*\{([^}]*)\}\s*(\w+)\s*;",
                                 text):
        values, nxt = {}, 0
        for item in (i.strip() for i in body.split(",")):
            if not item:
                continue
            m = re.match(r"(\w+)\s*(?:=\s*(\S+))?$", item)
            if not m:
                continue
            if m.group(2):
                nxt = int(m.group(2), 0)
            values[nxt] = m.group(1)
            nxt += 1
        enums[name] = values
    return enums


def cmd_crash(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    reason, *scratch = struct.unpack("<5I", read_memory(WATCHDOG_REASON, 20))
    print(f"watchdog reason 0x{reason:08x}; scratch 0-3: " +
          " ".join(f"0x{v:08x}" for v in scratch))
    names = ("bootCause", "bootPhase", "bootPc", "bootLr", "bootSp")
    syms = elf_symbols(elf, *names)
    if len(syms) < len(names):
        for i, v in enumerate(scratch):
            where = resolve_address(elf, v)
            if where:
                print(f"  scratch {i}: {where}")
        return 0
    values = {}
    for name in names:
        addr, size = syms[name]
        values[name] = int.from_bytes(read_memory(addr, size or 4), "little")
    enums = {}
    for header in glob.glob(os.path.join(INCLUDE_DIR, "*.h")):
        enums.update(header_enums(header))
    cause = enums.get("health_boot_t", {}).get(values["bootCause"],
                                               str(values["bootCause"]))
    print(f"last boot cause: {cause}")
    if cause.endswith("HANG"):
        phase = enums.get("health_phase_t", {}).get(values["bootPhase"],
                                                    str(values["bootPhase"]))
        print(f"  phase: {phase}")
    for label, name in (("pc", "bootPc"), ("lr", "bootLr"), ("sp", "bootSp")):
        v = values[name]
        if v:
            where = resolve_address(elf, v)
            print(f"  {label} 0x{v:08x}" + (f"  {where}" if where else ""))
    return 0


def gdb_command() -> str:
    gdb_dir = os.environ.get("ARM_GDB_PATH")
    candidates = [os.path.join(gdb_dir, "bin", "arm-none-eabi-gdb")] if gdb_dir else []
    candidates.append(shutil.which("arm-none-eabi-gdb") or "")
    for c in candidates:
        if c and os.access(c, os.X_OK):
            return c
    raise SwdError("no arm-none-eabi-gdb: set ARM_GDB_PATH")


# OpenOCD's GDB server without its memory map. GDB asks for the map when it
# attaches, and building it probes the flash bank through the bootrom, which
# leaves the flash out of XIP: the firmware stops at its next cache miss and
# the probe reads zeros from flash until a reset. GDB gets the flash as a
# read-only region of its own instead, so a breakpoint there is still a
# hardware one.
GDB_SERVER_NO_MAP = ["-c", "gdb_memory_map disable"]
GDB_FLASH_REGION = ["-ex", "set mem inaccessible-by-default off",
                    "-ex", "mem 0x10000000 0x11000000 ro"]


def cmd_postmortem(args: argparse.Namespace) -> int:
    elf = matching_elf(args.elf)
    gdb = gdb_command()
    server = openocd_command() + [
        "-c", f"gdb_port {GDB_PORT}", "-c", "tcl_port disabled",
        "-c", "telnet_port disabled"] + GDB_SERVER_NO_MAP
    if args.leave_halted:
        # OpenOCD resumes the target when GDB detaches, unless told not to.
        for core in CORES:
            server += ["-c", f"{core} configure -event gdb-detach {{}}"]
    proc = subprocess.Popen(server, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if f"port {GDB_PORT}" in line:
                ready = True
                break
        if not ready:
            raise SwdError("OpenOCD did not start its GDB server")
        present = elf_symbols(elf, *POSTMORTEM_VARIABLES)
        gdb_cmds = ["set pagination off", "set confirm off",
                    "set print pretty on",
                    f"target extended-remote localhost:{GDB_PORT}",
                    "monitor halt",
                    "echo \\n=== threads (one per core)\\n", "info threads",
                    "echo \\n=== backtraces\\n", "thread apply all bt",
                    "echo \\n=== registers (current core)\\n",
                    "info registers",
                    "echo \\n=== watchdog reason and scratch 0-3\\n",
                    f"x/5xw 0x{WATCHDOG_REASON:08x}"]
        if present:
            gdb_cmds.append("echo \\n=== variables\\n")
            for name in POSTMORTEM_VARIABLES:
                if name in present:
                    gdb_cmds += [f"echo {name} = ", f"output {name}",
                                 "echo \\n"]
        if not args.leave_halted:
            gdb_cmds.append("monitor resume")
        gdb_cmds.append("detach")
        argv = [gdb, "-nx", "-batch", elf]
        for c in gdb_cmds:
            argv += ["-ex", c]
        out = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        print(out.stdout.rstrip())
        errors = [l for l in out.stderr.splitlines()
                  if l.strip() and "warning" not in l.lower()]
        if errors:
            print("\n".join(errors[-5:]), file=sys.stderr)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if args.leave_halted:
        halted = [c for c in CORES if read_word(DHCSR, c) & DHCSR_S_HALT]
        print("\nleft halted: " + (", ".join(halted) or "nothing") +
              " (swd.py resume to continue)")
        return 0
    # OpenOCD leaves debug mode enabled (C_DEBUGEN) after GDB detaches; clear
    # it on both cores so the RP runs exactly as before the halt.
    return cmd_resume(args)


GDB_DIR = os.path.join(HERE, "gdb")


def cmd_gdb(args: argparse.Namespace) -> int:
    """GDB on the running RP, through the same OpenOCD server as postmortem.

    Loads tools/dev/gdb/rom-emulator.gdb (the app's commands) and each
    --script. Without --seconds it is an interactive session. With it, the
    scripts set their breakpoints and the RP runs for that long, printing what
    each breakpoint's commands print; then GDB interrupts, resumes both cores
    and detaches, so the RP is left running as it was."""
    elf = matching_elf(args.elf)
    gdb = gdb_command()
    server = openocd_command() + [
        "-c", f"gdb_port {GDB_PORT}", "-c", f"tcl_port {TCL_PORT}",
        "-c", "telnet_port disabled"] + GDB_SERVER_NO_MAP
    proc = subprocess.Popen(server, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.monotonic() + 10
        ready = False
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            if f"port {GDB_PORT}" in line:
                ready = True
                break
        if not ready:
            raise SwdError("OpenOCD did not start its GDB server")
        # Other swd.py commands reach the probe through this OpenOCD's TCL
        # server while GDB is attached (tcl_server()); drain its output so it
        # never blocks on a full pipe.
        threading.Thread(target=proc.stdout.read, daemon=True).start()
        argv = [gdb, "-nx", "-q", elf,
                "-ex", "set pagination off", "-ex", "set confirm off",
                *GDB_FLASH_REGION,
                "-ex", f"target extended-remote localhost:{GDB_PORT}",
                # The timer keeps running while GDB holds the cores in debug
                # (swd.py debug-pause off): with it paused, every sleep in the
                # firmware waits for the session to end.
                "-ex", f"monitor mww 0x{TIMER_DBGPAUSE:08x} 0",
                "-x", os.path.join(GDB_DIR, "rom-emulator.gdb")]
        for script in args.script:
            argv += ["-x", script]
        if args.seconds is None:
            return subprocess.call(argv)
        gdb_proc = subprocess.Popen(argv + ["-ex", "continue"],
                                    stdin=subprocess.PIPE, text=True)
        time.sleep(args.seconds)
        gdb_proc.send_signal(signal.SIGINT)
        time.sleep(1)
        gdb_proc.communicate("monitor resume\ndetach\nquit\n", timeout=30)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    # As after postmortem: clear the debug enable GDB leaves on both cores.
    return cmd_resume(args)


PSM_WDSEL = 0x40010008
PSM_WDSEL_ALL_BUT_OSCILLATORS = 0x0001FFFC
WATCHDOG_CTRL = 0x40058000
WATCHDOG_CTRL_TRIGGER = 0x80000000


def chip_reset() -> None:
    """Reset the whole chip at once, the way the watchdog does.

    Not OpenOCD's own `reset`: its multi-core sequence touches core 1 again a
    few milliseconds after core 0 has started. Up to v1.1.0 the firmware had
    launched core 1 (the SELECT watcher) by then, and killing it mid-trace
    left the SDK's stdio mutex held, so every piece of debug output waited out
    the 1 s PICO_STDIO_DEADLOCK_TIMEOUT_MS (a 0.7 s boot took 220 s). SELECT
    now runs on core 0, but a watchdog-style reset still restarts both cores
    together, like a power-on reset, and leaves the debugger nothing to do."""
    openocd(f"mww 0x{PSM_WDSEL:08x} 0x{PSM_WDSEL_ALL_BUT_OSCILLATORS:08x}",
            f"mww 0x{WATCHDOG_CTRL:08x} 0x{WATCHDOG_CTRL_TRIGGER:08x}",
            check=False)
    time.sleep(0.5)
    # `program` leaves the cores halted; make sure a halt does not survive.
    try:
        if any(read_word(DHCSR, core) & DHCSR_S_HALT for core in CORES):
            commands = []
            for core in CORES:
                commands += [f"targets {core}",
                             f"mww 0x{DHCSR:08x} 0x{DHCSR_RELEASE:08x}"]
            openocd(*commands)
    except SwdError:
        pass  # the debug port can blink while the chip restarts


DMA_BASE = 0x50000000
DMA_CHANNELS = 12
DMA_AL1_CTRL = 0x10      # CTRL alias that does not trigger the channel
DMA_CHAN_ABORT = 0x50000444
PIO_CTRL = (0x50200000, 0x50300000)


def quiesce_commands() -> list[str]:
    """Halt both cores, then stop every PIO state machine and DMA channel.

    Halting the cores does not stop the RP2040's DMA: the ROM3 capture ring
    keeps writing bus samples into RAM for as long as the ST touches the
    cartridge. A flash write that stages its data in that RAM then programs
    bus samples instead of code (it happened: 20 bytes of an image arrived as
    0x80xx words while the ST was reset-looping). PIO first, so no new
    DREQ arrives; then clear each channel's enable without triggering it, then
    abort whatever is in flight."""
    cmds = []
    for core in CORES:
        cmds += [f"targets {core}", "halt"]
    cmds += [f"mww 0x{ctrl:08x} 0" for ctrl in PIO_CTRL]
    cmds += [f"mww 0x{DMA_BASE + 0x40 * n + DMA_AL1_CTRL:08x} 0"
             for n in range(DMA_CHANNELS)]
    cmds += [f"mww 0x{DMA_CHAN_ABORT:08x} 0x{(1 << DMA_CHANNELS) - 1:x}",
             f"targets {CORES[0]}"]
    return cmds


def cmd_program(args: argparse.Namespace) -> int:
    # Not OpenOCD's `program`: it resets with OpenOCD's own sequence first,
    # which leaves DMA running (see quiesce_commands) and touches core 1.
    out = openocd(*quiesce_commands(), f"flash write_image erase {args.elf}",
                  f"verify_image {args.elf}", check=False, work_area=True)
    if not re.search(r"verified \d+ bytes", out):
        chip_reset()
        raise SwdError("flash write did not verify: " + " / ".join(
            l.strip() for l in out.splitlines() if l.startswith("Error"))[-300:])
    chip_reset()
    print(f"flashed {args.elf}")
    return 0


def cmd_reset(args: argparse.Namespace) -> int:
    chip_reset()
    print("chip reset (watchdog-style, both cores together)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="swd.py", description="Read and check a running RP2040 over SWD.")
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("running", help="wait until the RP runs the ELF")
    r.add_argument("elf")
    r.add_argument("--timeout", type=float, default=30.0)
    r.set_defaults(func=cmd_running)

    v = sub.add_parser("verify", help="compare the flash with the ELF")
    v.add_argument("elf")
    v.set_defaults(func=cmd_verify)

    b = sub.add_parser("build-id", help="read the build ID from flash")
    b.add_argument("elf", nargs="*")
    b.set_defaults(func=cmd_build_id)

    m = sub.add_parser("read", help="dump a memory range to a file")
    m.add_argument("address")
    m.add_argument("length")
    m.add_argument("outfile")
    m.set_defaults(func=cmd_read)

    g = sub.add_parser("program", help="flash the ELF and reset")
    g.add_argument("elf")
    g.set_defaults(func=cmd_program)

    rs = sub.add_parser("resume", help="release both cores from a debug halt")
    rs.set_defaults(func=cmd_resume)

    rst = sub.add_parser("reset", help="reset the whole chip, watchdog-style")
    rst.set_defaults(func=cmd_reset)

    cn = sub.add_parser("counters", help="the command channel's counters, "
                        "without halting")
    cn.add_argument("--elf")
    cn.set_defaults(func=cmd_counters)

    rg = sub.add_parser("ring", help="the ST's commands, decoded from the "
                        "capture ring without halting")
    rg.add_argument("--elf")
    mark = rg.add_mutually_exclusive_group()
    mark.add_argument("--mark", action="store_true",
                      help="remember the ring's position and print nothing")
    mark.add_argument("--since-mark", action="store_true",
                      help="only what the ST sent since the last --mark")
    rg.set_defaults(func=cmd_ring)

    hp = sub.add_parser("heap", help="heap size, peak and free space")
    hp.add_argument("--elf")
    hp.add_argument("--watch", type=float, metavar="SECONDS",
                    help="sample again every SECONDS until Ctrl-C")
    hp.add_argument("--csv", help="also append each sample to this CSV file")
    hp.set_defaults(func=cmd_heap)

    se = sub.add_parser("select", help="press the SELECT button")
    se.add_argument("press", choices=("short", "long", "release"))
    se.add_argument("--hold-ms", type=int)
    se.add_argument("--force", action="store_true",
                    help="allow a long press")
    se.set_defaults(func=cmd_select)

    k = sub.add_parser("key", help="send a keystroke as the ST would")
    k.add_argument("char", help="one character, or up, down, left, right, "
                   "esc, return, space")
    k.add_argument("--shift", action="store_true")
    k.add_argument("--scan", type=int, default=0)
    k.add_argument("--elf")
    k.set_defaults(func=cmd_key)

    t = sub.add_parser("type", help="type a line as the ST would, quickly")
    t.add_argument("text")
    t.add_argument("--enter", action="store_true", help="then RETURN")
    t.add_argument("--elf")
    t.set_defaults(func=cmd_type)

    ap = sub.add_parser("app", help="send an app command (DEVHOOKS_APP_*)")
    ap.add_argument("name")
    ap.add_argument("words", nargs="*", help="16-bit payload words")
    ap.add_argument("--elf")
    ap.set_defaults(func=cmd_app)

    ij = sub.add_parser("inject", help="inject a protocol command")
    ij.add_argument("command_id")
    ij.add_argument("words", nargs="*")
    ij.add_argument("--elf")
    ij.set_defaults(func=cmd_inject)

    cr = sub.add_parser("crash", help="explain the last reboot")
    cr.add_argument("--elf")
    cr.set_defaults(func=cmd_crash)

    pm = sub.add_parser("postmortem", help="halt, dump backtraces, resume")
    pm.add_argument("--elf")
    pm.add_argument("--leave-halted", action="store_true")
    pm.set_defaults(func=cmd_postmortem)

    gd = sub.add_parser("gdb", help="GDB on the running RP, with the app's "
                        "commands (tools/dev/gdb); --seconds for a timed, "
                        "unattended run of breakpoint scripts")
    gd.add_argument("--script", action="append", default=[],
                    help="a GDB script to load (repeatable)")
    gd.add_argument("--seconds", type=float,
                    help="run this long, then resume both cores and detach")
    gd.add_argument("--elf")
    gd.set_defaults(func=cmd_gdb)

    wn = sub.add_parser("window", help="compare the cartridge window and "
                        "ROM_TEMP with a ROM file, as the ST reads them")
    wn.add_argument("file")
    wn.add_argument("--no-flash", action="store_true",
                    help="check the window only, not ROM_TEMP")
    wn.add_argument("--elf")
    wn.set_defaults(func=cmd_window)

    stk = sub.add_parser("stack", help="paint both stack banks, or report "
                         "how deep the stacks have gone since")
    stk.add_argument("action", choices=("paint", "peak"))
    stk.set_defaults(func=cmd_stack)

    dp = sub.add_parser("debug-pause", help="whether the RP's timer stops "
                        "while a core is halted in debug (on after every reset)")
    dp.add_argument("state", choices=("on", "off"))
    dp.set_defaults(func=cmd_debug_pause)

    bb = sub.add_parser("boot-break", help="restart the RP, stop core 0 at "
                        "source lines on the way up, set registers, run on")
    bb.add_argument("stops", nargs="+", metavar="STOP",
                    help="LOCATION[@REG=VALUE,...] in boot order; LOCATION is "
                    "FILE:LINE (emul.c:197) or an address (0x...)")
    bb.add_argument("--via", default="main", help="a flash function reached "
                    "after the boot copies RAM code (for a location in RAM)")
    bb.add_argument("--park-core1", action="store_true", help="leave core 1 "
                    "halted and the timer running (v2.1.2: before a flash write)")
    bb.add_argument("--timeout", type=float, default=20)
    bb.add_argument("--elf")
    bb.set_defaults(func=cmd_boot_break)

    sr = sub.add_parser("st-reset", help="reboot the ST from its setup menu, "
                        "as the menu itself does")
    sr.add_argument("--wait", type=float, default=0,
                    help="seconds to wait for the ST to boot back into the menu")
    sr.add_argument("--offset", help="window offset of the word the ST's "
                    "loop polls, when it runs another firmware's cartridge "
                    "code (0x2000 for the template's sentinel)")
    sr.add_argument("--rom", action="store_true", help="the RP serves a ROM, "
                    "not the setup menu: with --wait, booting through the "
                    "cartridge is enough")
    sr.add_argument("--elf")
    sr.set_defaults(func=cmd_st_reset)

    sc = sub.add_parser("screen", help="render the framebuffer as a PNG")
    sc.add_argument("out")
    sc.add_argument("--elf")
    sc.add_argument("--scale", type=int, default=2)
    sc.set_defaults(func=cmd_screen)

    tx = sub.add_parser("text", help="print the terminal screen as text")
    tx.add_argument("--elf")
    tx.set_defaults(func=cmd_text)

    sh = sub.add_parser("shared", help="print the shared variables")
    sh.add_argument("--elf")
    sh.add_argument("--all", action="store_true",
                    help="also print unnamed slots that are not zero")
    sh.set_defaults(func=cmd_shared)
    return p


def main() -> int:
    args = build_parser().parse_args()
    try:
        return args.func(args)
    except (SwdError, subprocess.CalledProcessError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
