#!/usr/bin/env bash
# Starts a multiplayer test pair on this machine: one host and one client,
# each with its own save file (host.sav / client.sav).
#
# Usage: tools/mp/run_pair.sh [extra game args...]
#   Extra args are passed to both instances (e.g. --scale 2).
#
# Environment overrides:
#   EXE       game executable       (default: <repo>/pokeemerald64)
#   SAVE_DIR  where the saves live  (default: <repo>)
#   PORT      UDP port              (default: 27500)
#
# If host.sav / client.sav don't exist yet and <repo>/pokeemerald.sav does,
# it is copied to create them. Existing saves are never overwritten.
#
# See docs/multiplayer/dev-harness.md.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${EXE:-$ROOT/pokeemerald64}"
SAVE_DIR="${SAVE_DIR:-$ROOT}"
PORT="${PORT:-27500}"

if [ ! -x "$EXE" ]; then
    echo "Executable not found: $EXE" >&2
    echo "Build it with 'make linux', or set EXE=/path/to/pokeemerald64." >&2
    exit 1
fi

mkdir -p "$SAVE_DIR"

for save in host.sav client.sav; do
    if [ ! -e "$SAVE_DIR/$save" ] && [ -e "$ROOT/pokeemerald.sav" ]; then
        echo "Creating $SAVE_DIR/$save from $ROOT/pokeemerald.sav"
        cp "$ROOT/pokeemerald.sav" "$SAVE_DIR/$save"
    fi
done

pids=()
cleanup() {
    for pid in ${pids[@]+"${pids[@]}"}; do
        kill "$pid" 2>/dev/null || true
    done
}
trap cleanup INT TERM

echo "Starting host   (save: $SAVE_DIR/host.sav, port $PORT)"
"$EXE" --save "$SAVE_DIR/host.sav" --host "$PORT" --mp-debug "$@" &
pids+=("$!")

# Give the host a moment to start listening before the client connects.
sleep 1

echo "Starting client (save: $SAVE_DIR/client.sav, joining 127.0.0.1:$PORT)"
"$EXE" --save "$SAVE_DIR/client.sav" --join "127.0.0.1:$PORT" --mp-debug "$@" &
pids+=("$!")

wait || true
