#!/bin/bash

# Fail fast. Without this a failed step was stepped over: the RP build went on
# with the committed target_firmware.h, or build.sh copied a UF2 an earlier run
# had left in rp/dist, and still exited 0 with a JSON announcing the new
# version. `-u` is not set: the argument checks below test unset arguments.
# Whatever fails after dist/ is created also empties it, so no UF2 or JSON
# that looks like a good build is left behind.
set -Eeo pipefail
SCRIPT_DIR=$(dirname "$(realpath "$0")")
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2; rm -rf "$SCRIPT_DIR/dist"' ERR
die() {
    echo "ERROR: ${BASH_SOURCE[0]}: line ${BASH_LINENO[0]}: $*" >&2
    rm -rf "$SCRIPT_DIR/dist"
    exit 1
}
cd "$SCRIPT_DIR"

# Set the dist directory. Delete previous contents first, so a build that
# fails at any later step, its arguments included, leaves no UF2 or JSON behind
echo "Delete previous dist directory"
rm -rf dist
mkdir dist

# Ensure all required arguments are provided
if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
    echo "Usage: $0 <board_type> <build_type> <app_uuid_key>"
    echo "Example: $0 pico_w release|debug 123e4567-e89b-12d3-a456-426614174000"
    die "missing arguments"
fi

# Board type, case-insensitive. Only the Pico W: the app needs its Wi-Fi for
# the catalog and the downloads, and without the CYW43 stack httpc does not
# compile (httpc.h includes pico/async_context.h).
BOARD_TYPE=$(echo "$1" | tr '[:upper:]' '[:lower:]')
case "$BOARD_TYPE" in
    pico_w) ;;
    *)
        die "unknown board type '$1'. Use pico_w."
        ;;
esac
export BOARD_TYPE
echo "Board type: $BOARD_TYPE"

# Build type, case-insensitive. debug is the same build with DPRINTF traces on
# the UART console, on the RP, and _DEBUG=1 in the m68k assembly.
BUILD_TYPE=$(echo "$2" | tr '[:upper:]' '[:lower:]')
case "$BUILD_TYPE" in
    release) DEBUG_MODE=0 ;;
    debug) DEBUG_MODE=1 ;;
    *)
        die "unknown build type '$2'. Use release or debug."
        ;;
esac
export BUILD_TYPE DEBUG_MODE
echo "Build type: $BUILD_TYPE (DEBUG_MODE=$DEBUG_MODE)"

# HTTPS downloads: APP_DOWNLOAD_HTTPS=1 builds TLS in (rp/src/CMakeLists.txt);
# unset or 0 builds HTTP only.
export APP_DOWNLOAD_HTTPS=${APP_DOWNLOAD_HTTPS:-0}
case "$APP_DOWNLOAD_HTTPS" in
    0|1) ;;
    *)
        die "APP_DOWNLOAD_HTTPS must be 0 or 1, not '$APP_DOWNLOAD_HTTPS'."
        ;;
esac
echo "HTTPS downloads: APP_DOWNLOAD_HTTPS=$APP_DOWNLOAD_HTTPS"

# The app description is needed at the end: check it before building.
if [ ! -f desc/app.json ]; then
    die "desc/app.json not found."
fi

# Copy the version.txt to each project. target/atarist/ is where the m68k
# Makefile reads it.
echo "Copy version.txt to each project"
cp version.txt rp/
cp version.txt target/
cp version.txt target/atarist/

# Display the version information. Not exported: the stcmd that
# atarist-toolkit-docker's installer writes uses $VERSION as its Docker image
# tag, and there is no image for the firmware's version.
VERSION=$(cat version.txt)
echo "Version: $VERSION"

# One date for both images, unless the caller sets one: with a fixed
# RELEASE_DATE two builds of one commit are byte-identical.
export RELEASE_DATE=${RELEASE_DATE:-$(date +"%Y-%m-%d %H:%M:%S")}
echo "Release date: $RELEASE_DATE"

# Set the APP_UUID_KEY of the app to be built
export APP_UUID_KEY=$3
echo "App UUID Key: $APP_UUID_KEY"

# Build the project in the target architecture. target/atarist/build.sh
# regenerates rp/src/include/target_firmware.h and fails if it cannot.
echo "Building target project"
(cd target/atarist && ./build.sh "$SCRIPT_DIR/target/atarist" release "$DEBUG_MODE")
echo "Done building target project"

# Build the rp project in the RP architecture
echo "Building rp project"
(cd rp && ./build.sh "$BOARD_TYPE" "$BUILD_TYPE")
if [ "$BUILD_TYPE" = "release" ]; then
    cp "rp/dist/rp-$BOARD_TYPE.uf2" dist/rp.uf2
else
    cp "rp/dist/rp-$BOARD_TYPE-$BUILD_TYPE.uf2" dist/rp.uf2
fi
echo "Done building rp project"

# Calculate the md5sum of the generated rp.uf2 file. Stock macOS has md5, not
# md5sum.
if command -v md5sum >/dev/null 2>&1; then
    MD5_HASH=$(md5sum dist/rp.uf2 | cut -d ' ' -f 1)
else
    MD5_HASH=$(md5 -q dist/rp.uf2)
fi
echo "$MD5_HASH  dist/rp.uf2" > dist/rp.uf2.md5sum

# Show the md5sum of the generated rp.uf2 file
echo "md5sum of the generated rp.uf2 file:"
cat dist/rp.uf2.md5sum

# Rename the file to the standard name <APP_UUID>.uf2
mv dist/rp.uf2 "dist/$APP_UUID_KEY.uf2"

# Copy the app.json file to the dist directory
cp desc/app.json dist/

# Use portable sed for Linux and macOS
if [ "$(uname)" = "Darwin" ]; then
    sed -i '' "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i '' "s/<BINARY_MD5_HASH>/$MD5_HASH/g" dist/app.json
    sed -i '' "s/<APP_VERSION>/$VERSION/g" dist/app.json
else
    sed -i "s/<APP_UUID>/$APP_UUID_KEY/g" dist/app.json
    sed -i "s/<BINARY_MD5_HASH>/$MD5_HASH/g" dist/app.json
    sed -i "s/<APP_VERSION>/$VERSION/g" dist/app.json
fi

mv "dist/$APP_UUID_KEY.uf2" "dist/$APP_UUID_KEY-$VERSION.uf2"

# Show the content of the $APP_UUID_KEY.json file
echo "Content of the $APP_UUID_KEY.json file:"
mv dist/app.json "dist/$APP_UUID_KEY.json"
cat "dist/$APP_UUID_KEY.json"

# Done
exit 0
