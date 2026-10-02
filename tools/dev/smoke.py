#!/usr/bin/env python3
"""A whole session on the hardware, driven through the Debug Probe.

    python3 tools/dev/smoke.py [--rom selfcheck.img] [--images DIR]
                               [--catalog] [--hands] [--elf ELF]

With the RP in its setup menu, it types into the menu (swd.py key, which uses
the devhooks mailbox when the firmware has one and the command slot when it
does not), reads the terminal and the ST's screen, and checks what happened:

  menu       the menu comes up
  st-reset   the ST reboots (swd.py st-reset) and comes back to the menu;
             skipped when nothing on the ST is listening
  browse     [B]rowse lists the card; with the cursor keys a page forward
             and back
  select     the marker moved onto the ROM given with --rom, its details
  launch     RETURN on its details: the RP restarts in ROM mode and the window holds
             exactly that file (swd.py window, against --images/<rom>)
  verdict    the self-check's verdict from the ROM3 capture (debug builds;
             only for selfcheck.img)
  select-back  after --rom-seconds, a SELECT press brings the setup menu back
  catalog    (--catalog) [D]ownload lists the catalog, ESC leaves it
  st-back    the ST reboots into the menu (swd.py st-reset, through the
             agent the self-check left); skipped when nothing listens

Between steps it fails the run if the RP restarted when nothing asked it to
(its microsecond timer went backwards) or the heap's headroom fell under
--heap-floor bytes. With --hands it also asks for the steps only a person can
do (a cold power-on of the ST) and waits for them; without it they are
reported as skipped. Each step's screen goes to tools/dev/logs/smoke-<time>/
as a PNG, and the results to report.json there.

The ROM must be on the card: fill_card.py puts it there through the
device's own download.

--park-core1 is for firmware that watches SELECT on core 1 (v2.1.2): core 1
runs from flash, and a settings save on core 0 erases flash under it, which
locks both cores up. Parked, core 1 is held halted by the probe, and the
timer is kept running (swd.py debug-pause off), so the rest of the session can
be checked. SELECT in the setup menu is then dead; in ROM mode core 0 watches
it. Firmware on the template never starts core 1.
"""

from __future__ import annotations

import argparse
import contextlib
import datetime
import io
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import swd  # noqa: E402  (same folder)

TIMERAWL = 0x40054028       # the RP2040's microsecond timer, low word


class Session:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.elf = swd.matching_elf(args.elf)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.out = os.path.join(HERE, "logs", f"smoke-{stamp}")
        os.makedirs(self.out, exist_ok=True)
        self.results: list[dict] = []
        self.last_timer = self.timer()
        self.reset_expected = False

    # --- the probe ---------------------------------------------------------

    def timer(self) -> int:
        return swd.read_word(TIMERAWL)

    def text(self) -> str:
        sym = swd.elf_symbols(self.elf, "screen").get("screen")
        if not sym or not sym[1]:
            return ""
        width = swd.include_defines().get("TERM_SCREEN_SIZE_X", 40)
        data = swd.read_memory(sym[0], sym[1])
        return "\n".join(
            "".join(chr(c) if 32 <= c < 127 else " " for c in
                    data[y * width:(y + 1) * width]).rstrip()
            for y in range(sym[1] // width))

    def key(self, char: str) -> None:
        defs = swd.include_defines()
        command_id = (defs["APP_TERMINAL"] << 8) | defs["APP_TERMINAL_KEYSTROKE"]
        swd.send_protocol(self.elf, command_id, [ord(char) & 0xFFFF, 0])

    def press(self, name: str) -> None:
        """A key by name (swd.NAMED_KEYS): the cursor keys, esc, return."""
        defs = swd.include_defines()
        command_id = (defs["APP_TERMINAL"] << 8) | defs["APP_TERMINAL_KEYSTROKE"]
        ascii_code, scan = swd.NAMED_KEYS[name]
        param = ascii_code | (scan << defs["TERM_KEYBOARD_SCAN_SHIFT"])
        swd.send_protocol(self.elf, command_id, [param & 0xFFFF, param >> 16])

    def mark(self, rom: str) -> bool:
        """Moves the list's marker onto rom, page by page."""
        while True:
            lines = [l for l in self.text().splitlines() if l[:2] in ("> ", "  ")]
            names = [l[2:].strip() for l in lines]
            marked = next((i for i, l in enumerate(lines) if l.startswith(">")), 0)
            target = next((i for i, n in enumerate(names)
                           if n == rom or (len(n) >= 20 and rom.startswith(n))), None)
            if target is not None:
                for _ in range(target - marked):
                    self.press("down")
                time.sleep(0.5)
                return True
            page = re.search(r"Page (\d+)/(\d+)", self.text())
            if not page or page.group(1) == page.group(2):
                return False
            self.press("right")
            self.wait_for(f"Page {int(page.group(1)) + 1}/", 10)

    def typeline(self, line: str) -> None:
        for char in line + "\r":
            self.key(char)

    def wait_for(self, needle: str, timeout: float = 30) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                if needle in self.text():
                    return True
            except swd.SwdError:
                pass  # the RP may be restarting
            time.sleep(1)
        return False

    def screen(self, name: str) -> str:
        path = os.path.join(self.out, f"{len(self.results) + 1:02d}-{name}.png")
        try:
            swd.cmd_screen(argparse.Namespace(out=path, elf=self.elf, scale=2))
        except swd.SwdError as exc:
            return f"no screen: {exc}"
        return path

    def park_core1(self) -> None:
        # Core 1's own DHCSR: C_HALT | C_DEBUGEN. OpenOCD's `halt` would stop
        # both cores, which it treats as one SMP target.
        swd.openocd("mww 0x%08x 0x0" % swd.TIMER_DBGPAUSE,
                    f"targets {swd.CORES[1]}",
                    "mww 0x%08x 0x%08x" % (swd.DHCSR, swd.DHCSR_RELEASE | 0x3))

    def st_listening(self) -> bool:
        seen = swd.st_reads(self.elf)
        fb = swd.include_defines().get("DISPLAY_BUFFER_OFFSET", 0x8000)
        return (any(swd.AGENT_SIG_OFFSET <= o < swd.AGENT_SIG_OFFSET + 8 for o in seen)
                or sum(fb <= o < 0x10000 for o in seen) >= 2)

    def verdict(self) -> None:
        """The self-check's verdict, from the ROM3 capture a debug build runs
        in ROM mode (swd.py ring): $7F01 pass, $7F02 fail."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            swd.cmd_ring(argparse.Namespace(elf=self.elf, mark=False,
                                            since_mark=False))
        text = out.getvalue()
        if "0x7f02" in text:
            self.step("verdict", False, "the self-check reported a word that "
                      "did not match: " + text.strip().splitlines()[0])
        elif "0x7f01" in text:
            self.step("verdict", True, "the ST read the whole window back")
        else:
            self.skip("verdict", "nothing in the ROM3 capture (a release "
                      "build captures nothing in ROM mode, or the ST did not "
                      "run the ROM)")

    def select_short(self) -> None:
        defs = swd.include_defines()
        ctrl = swd.IO_BANK0 + 4 + 8 * defs["SELECT_GPIO"]
        normal = swd.read_word(ctrl) & ~(3 << swd.INOVER_SHIFT)
        pressed = normal | (swd.INOVER_HIGH << swd.INOVER_SHIFT)
        swd.openocd(f"mww 0x{ctrl:08x} 0x{pressed:08x}",
                    f"sleep {swd.SELECT_SHORT_MS}",
                    f"mww 0x{ctrl:08x} 0x{normal:08x}")

    # --- steps and checks --------------------------------------------------

    def health(self) -> str:
        """'' when fine, else why: an unrequested restart or a low heap."""
        problems = []
        now = self.timer()
        if now < self.last_timer and not self.reset_expected:
            problems.append("the RP restarted and nothing asked it to")
        self.last_timer, self.reset_expected = now, False
        snap = swd.heap_snapshot(self.elf)
        if snap.get("headroom", 1 << 30) < self.args.heap_floor:
            problems.append(f"heap headroom {snap['headroom']} bytes, under "
                            f"{self.args.heap_floor}")
        return "; ".join(problems)

    def step(self, name: str, ok: bool, detail: str = "",
             hands: bool = False) -> bool:
        problem = "" if hands else self.health()
        result = {"step": name, "ok": ok and not problem,
                  "detail": "; ".join(d for d in (detail, problem) if d),
                  "screen": self.screen(name) if not hands else ""}
        self.results.append(result)
        print(f"{'PASS' if result['ok'] else 'FAIL'}  {name}"
              + (f": {result['detail']}" if result["detail"] else ""), flush=True)
        return result["ok"]

    def skip(self, name: str, why: str) -> None:
        self.results.append({"step": name, "ok": None, "detail": why})
        print(f"SKIP  {name}: {why}", flush=True)

    def ask(self, what: str) -> bool:
        if not self.args.hands:
            return False
        input(f"\n>>> {what}, then press Return here... ")
        return True

    # --- the session -------------------------------------------------------

    def run(self) -> int:
        a = self.args
        if a.park_core1:
            self.park_core1()
        self.typeline("m")
        if not self.step("menu", self.wait_for("Select an option")):
            return self.finish()

        # Only while something on the ST listens: the setup menu, or the
        # remote reset agent the self-check cartridge leaves in its RAM.
        if self.st_listening():
            verdict = swd.cmd_st_reset(argparse.Namespace(
                wait=30, offset=None, rom=False, elf=self.elf))
            self.step("st-reset", verdict == 0,
                      "the ST read the cartridge header again, then the framebuffer")
        else:
            self.skip("st-reset", "nothing on the ST reads the cartridge: it "
                      "needs its reset button once")

        self.typeline("b")
        listed = self.wait_for("ROMs on the SD card") and self.wait_for("Page 1/")
        self.step("browse", listed)
        if not listed:
            return self.finish()
        # More than a page: turn it and back with the cursor keys.
        pages = re.search(r"Page \d+/(\d+)", self.text())
        if pages and int(pages.group(1)) > 1:
            self.press("right")
            forward = self.wait_for("Page 2/")
            self.press("left")
            self.step("page", forward and self.wait_for("Page 1/"))

        if not self.mark(a.rom):
            self.step("select", False, f"{a.rom} is not on the card")
            return self.finish()
        self.press("return")
        self.step("select", self.wait_for(f"ROM: {a.rom}"))
        self.reset_expected = True
        self.press("return")
        time.sleep(a.boot_seconds)
        rom_path = os.path.join(a.images, a.rom)
        verdict = swd.cmd_window(argparse.Namespace(
            file=rom_path, no_flash=False, elf=self.elf))
        self.step("launch", verdict == 0, f"window check against {rom_path}")

        # The ST runs the ROM to its end first: the self-check boots, reads
        # the window, holds its verdict 5 s and installs the agent. A SELECT
        # before that puts the setup menu under the code the ST is running.
        time.sleep(a.rom_seconds)
        if os.path.basename(a.rom) == "selfcheck.img":
            self.verdict()
        self.reset_expected = True
        self.select_short()
        if a.park_core1:
            time.sleep(3)          # the RP restarts into setup mode
            self.park_core1()
        self.step("select-back", self.wait_for("Select an option",
                                                a.boot_seconds + 30))

        if a.catalog:
            self.typeline("d")
            self.step("catalog", self.wait_for("ROM catalog", 60)
                      and self.wait_for("Page 1/", 10))
            self.press("esc")

        if self.st_listening():
            verdict = swd.cmd_st_reset(argparse.Namespace(
                wait=30, offset=None, rom=False, elf=self.elf))
            self.step("st-back", verdict == 0,
                      "the ST rebooted into the menu")
        else:
            self.skip("st-back", "nothing on the ST reads the cartridge: it "
                      "needs its reset button once")

        if self.ask("Power the ST off, wait 5 s, power it on"):
            self.reset_expected = True
            self.step("cold power-on", self.wait_for("Select an option", 90),
                      hands=True)
        else:
            self.skip("cold power-on", "needs hands (--hands)")
        return self.finish()

    def finish(self) -> int:
        with open(os.path.join(self.out, "report.json"), "w") as f:
            json.dump({"elf": self.elf, "build_id": swd.elf_build_id(self.elf),
                       "steps": self.results}, f, indent=2)
        failed = [r for r in self.results if r["ok"] is False]
        print(f"\n{len(self.results)} steps, {len(failed)} failed; "
              f"report in {self.out}")
        return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    # The self-check by default: it leaves the remote reset agent on the ST,
    # so the next run can reboot it without hands.
    ap.add_argument("--rom", default="selfcheck.img")
    ap.add_argument("--images", default=os.path.join(HERE, "builds", "testserver"),
                    help="where the ROM files the card holds are on this machine")
    ap.add_argument("--catalog", action="store_true")
    ap.add_argument("--hands", action="store_true")
    ap.add_argument("--park-core1", action="store_true",
                    help="hold core 1 halted (firmware with a core-1 SELECT watcher)")
    ap.add_argument("--heap-floor", type=int, default=8192)
    ap.add_argument("--boot-seconds", type=float, default=5)
    ap.add_argument("--rom-seconds", type=float, default=15,
                    help="how long the launched ROM runs on the ST before "
                         "the SELECT press")
    ap.add_argument("--elf")
    args = ap.parse_args()
    try:
        return Session(args).run()
    except swd.SwdError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
