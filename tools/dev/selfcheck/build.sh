#!/bin/bash
# Assemble the self-check cartridge and wrap it in the 128 KB test pattern.
#
#   tools/dev/selfcheck/build.sh OUT_DIR
#
# Writes OUT_DIR/selfcheck.img (passes) and OUT_DIR/selfcheck-broken.img (one
# word changed in ROM3, so it fails there). Assembles with vasm in the AtariST
# toolkit image (stcmd), as target/atarist does.
set -Eeo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: build.sh OUT_DIR}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

export STCMD_NO_TTY=1 STCMD_QUIET=1
ST_WORKING_FOLDER="$HERE" stcmd vasm -Fbin -m68000 -devpac -spaces -quiet \
  -o selfcheck.bin selfcheck.s
python3 "$HERE/../make_rom_images.py" --selfcheck "$HERE/selfcheck.bin" "$OUT"
rm -f "$HERE/selfcheck.bin"
