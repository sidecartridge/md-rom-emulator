#!/usr/bin/env python3
"""Synthetic cartridge images whose every word can be checked.

    python3 tools/dev/make_rom_images.py OUT_DIR
    python3 tools/dev/make_rom_images.py --selfcheck CODE.BIN OUT_DIR

Each 16-bit word of an image holds its own word index in the 128 KB window,
XORed with 0xA55A so that a word's two bytes differ and a byte swap shows:
the word the ST reads at $FA0000 + 2*i is (i ^ 0xA55A) & 0xFFFF, stored
big-endian, as a cartridge image is on the card. `swd.py window` checks a
running RP against the same pattern (pattern_word below).

The set covers every path the ROM loader can take, including the ones it must
refuse. The same files, byte for byte, on every run:

    pattern-64k.img           64 KB, ROM4 only
    pattern-128k.rom          128 KB, ROM4 and ROM3
    PATTERN-128K.STC          STEEM image: 4 zero bytes, then 128 KB
    zero-head-128k.bin        128 KB whose first two words are zero, no header
    odd-40001.IMG             40,001 bytes: not a multiple of a flash sector
    oversize-132k.ROM         132 KB: must be refused, never launched before
                              the loader bounds its writes
    empty.bin                 0 bytes
    a-rom-image-whose-name-is-sixty-characters-long-for-test.img
                              64 KB, a long file name

With --selfcheck, CODE.BIN is the assembled self-check cartridge
(tools/dev/selfcheck): it is padded with the pattern to 128 KB as
selfcheck.img, and selfcheck-broken.img has one word changed at $FB1234, where
the check must fail.
"""

import os
import sys

WINDOW = 128 * 1024
PATTERN_XOR = 0xA55A


def pattern_word(index: int) -> int:
    """The word the ST reads at $FA0000 + 2 * index."""
    return (index ^ PATTERN_XOR) & 0xFFFF


def pattern(length: int) -> bytes:
    """`length` bytes of the pattern from the start of the window, as a file."""
    words = (length + 1) // 2
    data = bytearray()
    for i in range(words):
        data += pattern_word(i).to_bytes(2, "big")
    return bytes(data[:length])


LONG_NAME = "a-rom-image-whose-name-is-sixty-characters-long-for-test.img"

IMAGES = {
    "pattern-64k.img": lambda: pattern(64 * 1024),
    "pattern-128k.rom": lambda: pattern(WINDOW),
    "PATTERN-128K.STC": lambda: bytes(4) + pattern(WINDOW),
    "zero-head-128k.bin": lambda: bytes(4) + pattern(WINDOW)[4:],
    "odd-40001.IMG": lambda: pattern(40001),
    "oversize-132k.ROM": lambda: pattern(WINDOW) + pattern(4 * 1024),
    "empty.bin": lambda: b"",
    LONG_NAME: lambda: pattern(64 * 1024),
    # The files make_catalog.py --cases lists, so their downloads succeed:
    # a 60-character catalog name, an entry with empty fields, and one whose
    # name and description carry commas and quotes.
    "a-catalog-entry-whose-name-is-sixty-characters-long-test.img":
        lambda: pattern(64 * 1024),
    "empty-fields.img": lambda: pattern(64 * 1024),
    "commas-and-quotes.img": lambda: pattern(64 * 1024),
}


SELFCHECK_PATTERN_FROM = 0x1000  # PATTERN_FROM in selfcheck.s
BROKEN_OFFSET = 0x11234         # $FB1234, in ROM3


def selfcheck_images(code_path: str, out: str) -> None:
    with open(code_path, "rb") as f:
        code = f.read()
    if len(code) > SELFCHECK_PATTERN_FROM:
        raise SystemExit(f"{code_path} is {len(code)} bytes: the check starts "
                         f"at {SELFCHECK_PATTERN_FROM:#x}")
    image = bytearray(code + pattern(WINDOW)[len(code):])
    broken = bytearray(image)
    broken[BROKEN_OFFSET] ^= 0xFF
    for name, data in (("selfcheck.img", image),
                       ("selfcheck-broken.img", broken)):
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"{len(data):7d}  {name}")


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--selfcheck":
        os.makedirs(sys.argv[3], exist_ok=True)
        selfcheck_images(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, make in IMAGES.items():
        data = make()
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)
        print(f"{len(data):7d}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
