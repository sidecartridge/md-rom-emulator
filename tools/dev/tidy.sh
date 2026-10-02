#!/bin/bash
# clang-tidy on the RP firmware's sources, with the repository's .clang-tidy
# and the compile commands of an out-of-tree build (run
# `tools/dev/flash.sh <type> --build-only` first). The commands are
# arm-none-eabi-gcc's: clang parses them as ARMv6-M, against the toolchain's
# newlib headers (its sysroot), or every C library header goes missing and
# the checks run on a broken parse.
#
#   tools/dev/tidy.sh [--build debug|release] [FILE ...]
#
# FILE is relative to rp/src. Without one, the app's own sources are checked:
# the ones that are not the template's. health.c is md-devops', taken whole
# like the template's files: only this app's lines in it are held to the
# config (check them with `tidy.sh health.c`). Diagnostics are shown for the files
# given and their own headers (include/<name>.h), not for every header they
# include. The exit status is clang-tidy's: non-zero when anything was found.
#
# Environment: CLANG_TIDY (default: clang-tidy on the PATH, else Homebrew's
# llvm).

set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD=debug
if [ "${1:-}" = "--build" ]; then
    BUILD=$2
    shift 2
fi
FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
    FILES=(emul.c catalog.c navlist.c romstore.c ui.c)
fi

CLANG_TIDY=${CLANG_TIDY:-$(command -v clang-tidy || echo /usr/local/opt/llvm/bin/clang-tidy)}
COMMANDS="$REPO/tools/dev/builds/$BUILD"
if [ ! -f "$COMMANDS/compile_commands.json" ]; then
    echo "no $COMMANDS/compile_commands.json: run tools/dev/flash.sh $BUILD --build-only" >&2
    exit 2
fi

SYSROOT=$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' \
    "$(arm-none-eabi-gcc -print-sysroot)")
GCC_INCLUDE=$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' \
    "$(arm-none-eabi-gcc -print-file-name=include)")

# The headers of the files given: include/emul.h for emul.c, and so on.
names=$(for f in "${FILES[@]}"; do basename "$f" .c; done | paste -sd'|' -)
HEADERS=".*/rp/src/include/($names)\\.h\$"

SOURCES=()
for f in "${FILES[@]}"; do
    SOURCES+=("$REPO/rp/src/$f")
done

exec "$CLANG_TIDY" -p "$COMMANDS" --quiet --header-filter="$HEADERS" \
    --extra-arg=--target=armv6m-none-eabi \
    --extra-arg=--sysroot="$SYSROOT" \
    --extra-arg=-isystem"$GCC_INCLUDE" \
    --extra-arg=-isystem"$SYSROOT/include" \
    "${SOURCES[@]}"
