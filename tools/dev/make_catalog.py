#!/usr/bin/env python3
"""ROM catalogs (roms.csv) in the format of the public one.

    python3 tools/dev/make_catalog.py OUT.csv --dir DIR
    python3 tools/dev/make_catalog.py OUT.csv --synthetic 1000
    python3 tools/dev/make_catalog.py OUT.csv --dir DIR --cases
    python3 tools/dev/make_catalog.py OUT.csv --dir DIR --no-final-newline

The public catalog (http://roms.sidecartridge.com/roms.csv) has a header
    "URL","Name","Description","Tags","Size (KB)"
and one quoted row per ROM. URL is the file name, percent-encoded; the device
fetches it from the catalog's own host, at the root.

--dir DIR        one row per file in DIR (the reference images, for example)
--synthetic N    N rows named synthetic-NNNN.img; testserver.py serves any of
                 them as the 64 KB pattern image
--cases          add the rows the parser and the loader must survive: a
                 60-character name, a name with a path in it, empty fields, a
                 size over 128 KB, a description with commas and quotes
"""

import argparse
import csv
import io
import os
import sys
import urllib.parse

HEADER = ["URL", "Name", "Description", "Tags", "Size (KB)"]


def row(filename: str, name: str, description: str, tags: str,
        size_kb: str) -> list[str]:
    return [urllib.parse.quote(filename), name, description, tags, size_kb]


def rows_from_dir(path: str) -> list[list[str]]:
    rows = []
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if not os.path.isfile(full) or name.startswith("."):
            continue
        size_kb = (os.path.getsize(full) + 1023) // 1024
        rows.append(row(name, os.path.splitext(name)[0], "Test image.",
                        "test", str(size_kb)))
    return rows


def rows_synthetic(count: int) -> list[list[str]]:
    return [row(f"synthetic-{i:04d}.img", f"Synthetic ROM {i:04d}",
                "A generated entry for paging tests.", "test; synthetic", "64")
            for i in range(1, count + 1)]


def rows_cases() -> list[list[str]]:
    return [
        row("a-catalog-entry-whose-name-is-sixty-characters-long-test.img",
            "A sixty-character file name", "Long names must survive.",
            "test", "64"),
        row("../escape.img", "A name with a path", "Must be refused: it would "
            "write outside the ROM folder.", "test", "64"),
        row("empty-fields.img", "", "", "", ""),
        row("oversize-256k.img", "Too large", "Size says 256 KB: must be "
            "refused before downloading.", "test", "256"),
        row("commas-and-quotes.img", "Commas, \"quotes\"", "A description, "
            "with commas, and \"quotes\".", "test; csv", "64"),
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--dir")
    ap.add_argument("--synthetic", type=int, default=0)
    ap.add_argument("--cases", action="store_true")
    ap.add_argument("--no-final-newline", action="store_true")
    args = ap.parse_args()
    rows = []
    if args.dir:
        rows += rows_from_dir(args.dir)
    rows += rows_synthetic(args.synthetic)
    if args.cases:
        rows += rows_cases()
    buf = io.StringIO()
    writer = csv.writer(buf, quoting=csv.QUOTE_ALL, lineterminator="\n")
    writer.writerow(HEADER)
    writer.writerows(rows)
    text = buf.getvalue()
    if args.no_final_newline:
        text = text.rstrip("\n")
    with open(args.out, "w", newline="") as f:
        f.write(text)
    print(f"{len(rows)} entries in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
