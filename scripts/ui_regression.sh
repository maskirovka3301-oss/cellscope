#!/bin/sh
# Render the major CellScope UI states to PNGs for visual regression review.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build"}
APP="$BUILD/cellscope.app/Contents/MacOS/cellscope"
IMAGE=${1:-"$ROOT/docs/example.png"}
OUT=${OUT:-/tmp/cellscope-ui-regression}

echo "Building Debug app in $BUILD"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$BUILD" --parallel
ctest --test-dir "$BUILD" --output-on-failure

if [ ! -x "$APP" ]; then
    echo "Missing app binary after build: $APP" >&2
    exit 1
fi
if [ ! -f "$IMAGE" ]; then
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

for mode in main live best settings upload model remove symptom; do
    CELLSCOPE_DISABLE_KEYCHAIN=1 "$APP" --autoplay --image "$IMAGE" --readme-fixture "$mode" \
        --screenshot "$OUT/$mode.png" --screenshot-delay 1200 --run-ms 3000 \
        --no-modal --no-runtime --capture-dir "$OUT/capture-$mode" >/dev/null 2>&1 || {
        echo "FAILED: $mode" >&2
        exit 1
    }
    [ -s "$OUT/$mode.png" ] || { echo "EMPTY screenshot: $mode" >&2; exit 1; }
    echo "ok $mode -> $OUT/$mode.png"
done

CELLSCOPE_DISABLE_KEYCHAIN=1 CELLSCOPE_START_PAUSED=1 "$APP" --no-camera --no-modal --relaunched --no-runtime \
    --screenshot "$OUT/paused.png" --screenshot-delay 900 --run-ms 1600 \
    --capture-dir "$OUT/capture-paused" >/dev/null 2>&1 || {
    echo "FAILED: paused" >&2
    exit 1
}
[ -s "$OUT/paused.png" ] || { echo "EMPTY screenshot: paused" >&2; exit 1; }
echo "ok paused -> $OUT/paused.png"

# Keep the single README screenshot current after every UI regression run.
# The main fixture seeds simulated ADB samples and readings, presents
# docs/example.png as a live webcam, and mirrors its 2526.5744 MHz reading in
# LIVE DETECTION.
CELLSCOPE_DISABLE_KEYCHAIN=1 "$APP" --autoplay --image "$IMAGE" --readme-fixture main \
    --screenshot "$ROOT/docs/screenshot-main.png" --screenshot-delay 1200 --run-ms 3000 \
    --no-modal --no-runtime --capture-dir "$OUT/capture-readme" >/dev/null 2>&1 || {
    echo "FAILED: README screenshot" >&2
    exit 1
}
[ -s "$ROOT/docs/screenshot-main.png" ] || {
    echo "EMPTY screenshot: README" >&2
    exit 1
}
echo "ok README -> $ROOT/docs/screenshot-main.png"
echo "UI regression screenshots: $OUT"
