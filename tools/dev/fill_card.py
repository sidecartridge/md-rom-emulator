#!/usr/bin/env python3
"""Fill the device's microSD card from the catalog, through its own downloads.

    python3 tools/dev/fill_card.py [--entries 3,4,5] [--timeout 60]

With the app's catalog pointed at testserver.py (the HTTP_CATALOG setting) and
the setup menu on screen, this downloads catalog entries one by one the way a
user does, typing into the menu over the probe (swd.py key): [D]ownload, the
entry's number, Return. Each download's outcome comes from the debug console
(tools/dev/logs/console.log, so a debug build and console.py watch are
needed): the file the firmware wrote, or the error it reported. Without
--entries it takes every entry on the first page.

It follows the v2.1.2 menu ("[M]enu or ROM number>", "Press RETURN to load
the ROM."); the texts are in PROMPTS.
"""

import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import swd  # noqa: E402  (same folder)

LOG = os.path.join(HERE, "logs", "console.log")
PROMPTS = {"catalog": "ROM number>", "confirm": "Press RETURN to load",
           "menu": "Select an option"}
DONE_RE = re.compile(r"Written file [^\n]+|Error[^\n]*download[^\n]*|"
                     r"download_start\(\): Error[^\n]*", re.I)


class Device:
    def __init__(self) -> None:
        self.elf = swd.matching_elf(None)

    def typeline(self, line: str) -> None:
        defs = swd.include_defines()
        command_id = (defs["APP_TERMINAL"] << 8) | defs["APP_TERMINAL_KEYSTROKE"]
        for char in line + "\r":
            swd.send_protocol(self.elf, command_id, [ord(char), 0])

    def text(self) -> str:
        sym = swd.elf_symbols(self.elf, "screen")["screen"]
        data = swd.read_memory(sym[0], sym[1])
        return data.decode("latin-1").replace("\0", " ")

    def wait_for(self, needle: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.text():
                return True
            time.sleep(0.5)
        return False


def log_size() -> int:
    return os.path.getsize(LOG) if os.path.exists(LOG) else 0


def wait_log(start: int, timeout: float) -> str:
    """The first download outcome logged after byte `start`, or ''."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with open(LOG, errors="replace") as f:
            f.seek(start)
            match = DONE_RE.search(f.read())
        if match:
            return match.group(0)
        time.sleep(0.5)
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--entries", help="comma-separated entry numbers")
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    try:
        return fill(args)
    except swd.SwdError as exc:
        print(f"ERROR: {exc} (swd.py postmortem says why)", file=sys.stderr)
        return 1


def fill(args: argparse.Namespace) -> int:
    dev = Device()
    dev.typeline("m")
    dev.typeline("d")
    if not dev.wait_for(PROMPTS["catalog"], 20):
        print("ERROR: the catalog did not come up", file=sys.stderr)
        return 1
    # The terminal's rows are 40 characters with no newlines: "1. name".
    listed = [int(m.group(1)) for m in re.finditer(r"(\d+)\. \S", dev.text())]
    entries = ([int(n) for n in args.entries.split(",")] if args.entries
               else sorted(set(listed)))
    failed = 0
    for number in entries:
        dev.typeline("m")
        dev.typeline("d")
        if not dev.wait_for(PROMPTS["catalog"], 20):
            print(f"{number}: the catalog did not come up")
            failed += 1
            continue
        start = log_size()
        dev.typeline(str(number))
        if not dev.wait_for(PROMPTS["confirm"], 10):
            print(f"{number}: no entry page")
            failed += 1
            continue
        dev.typeline("")
        outcome = wait_log(start, args.timeout)
        print(f"{number}: {outcome or 'no outcome logged within the timeout'}")
        failed += not outcome.startswith("Written file")
    dev.typeline("m")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
