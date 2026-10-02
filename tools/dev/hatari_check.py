#!/usr/bin/env python3
"""Run cartridge images under Hatari and read what they print.

    python3 tools/dev/hatari_check.py IMAGE [--expect TEXT] [--tos 1.04 ...]
    python3 tools/dev/hatari_check.py --selfcheck DIR

Hatari is the reference for what a correct cartridge does, as in
md-drives-emulator's hatari_tests.py: each image boots as the cartridge
(--cartridge) on each TOS, with no GEMDOS drive (Hatari's own cartridge would
take the port) and the console copied to stdout (--conout 2). A run passes
when the output contains --expect within --timeout seconds.

--selfcheck DIR runs the two images tools/dev/selfcheck/build.sh writes:
selfcheck.img must print "self-check: PASS" and selfcheck-broken.img
"self-check: FAIL at $FB1234", on every TOS.

TOS images come from --tos-dir (default ~/mister_wkspc/TOS). TOS 1.00 is left
out: its console output does not reach --conout.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time
from collections import OrderedDict

DEFAULT_TOS_DIR = os.path.expanduser("~/mister_wkspc/TOS")
TOS_IMAGES = OrderedDict([
    ("1.02", ("tos102us.img", "st")),
    ("1.04", ("tos104us.img", "st")),
    ("1.06", ("tos106us.img", "ste")),
    ("1.62", ("TOS v1.62 (1990)(Atari Corp)(STE)(US)[b].img", "ste")),
    ("2.06", ("TOS v2.06 (1991)(Atari Corp)(Mega-STE)(US).img", "megaste")),
    ("3.06", ("TOS v3.06 (1991)(Atari Corp)(UK)(TT).img", "tt")),
    ("4.04", ("TOS v4.04 (19xx)(Atari Corp)(Falcon).img", "falcon")),
])


def run(image: str, tos: str, tos_dir: str, expect: str,
        timeout: float) -> tuple[bool, str]:
    """Boot `image` as the cartridge on `tos`; (passed, console output)."""
    name, machine = TOS_IMAGES[tos]
    with tempfile.TemporaryDirectory(prefix="hatari-check-") as work:
        config = os.path.join(work, "hatari.cfg")
        with open(config, "w") as f:
            f.write("[HardDisk]\nbUseHardDiskDirectory = FALSE\n")
        out_path = os.path.join(work, "console.txt")
        command = ["hatari", "--configfile", config,
                   "--tos", os.path.join(tos_dir, name), "--machine", machine,
                   "--memsize", "1", "--sound", "off", "--conout", "2",
                   "--fast-forward", "on", "--confirm-quit", "off",
                   "--cartridge", image]
        env = dict(os.environ, SDL_VIDEODRIVER="dummy")
        with open(out_path, "w") as out:
            proc = subprocess.Popen(command, env=env, stdout=out,
                                    stderr=subprocess.STDOUT)
            deadline = time.monotonic() + timeout
            found = False
            while time.monotonic() < deadline and proc.poll() is None:
                with open(out_path, errors="replace") as f:
                    if expect in f.read():
                        found = True
                        break
                time.sleep(0.5)
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        with open(out_path, errors="replace") as f:
            text = f.read()
    return found or expect in text, text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--expect", default="self-check: PASS")
    ap.add_argument("--selfcheck", metavar="DIR")
    ap.add_argument("--tos", action="append", choices=list(TOS_IMAGES))
    ap.add_argument("--tos-dir", default=DEFAULT_TOS_DIR)
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()
    if args.selfcheck:
        cases = [(os.path.join(args.selfcheck, "selfcheck.img"),
                  "self-check: PASS"),
                 (os.path.join(args.selfcheck, "selfcheck-broken.img"),
                  "self-check: FAIL at $FB1234")]
    elif args.image:
        cases = [(args.image, args.expect)]
    else:
        ap.error("give an IMAGE or --selfcheck DIR")
    failed = 0
    for image, expect in cases:
        for tos in args.tos or list(TOS_IMAGES):
            passed, text = run(image, tos, args.tos_dir, expect, args.timeout)
            line = next((l for l in text.splitlines() if "self-check" in l),
                        "(nothing printed)")
            print(f"{'PASS' if passed else 'FAIL'}  TOS {tos:4}  "
                  f"{os.path.basename(image)}: {line.strip()}")
            failed += not passed
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
