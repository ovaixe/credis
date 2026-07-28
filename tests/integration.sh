#!/bin/sh
# Starts credis-server on a scratch port, runs the end-to-end checks against it,
# and shuts it down again. Invoked by CTest as the "integration" test, and
# usable directly:  tests/integration.sh build/credis-server
set -eu

SERVER="${1:-build/credis-server}"
PORT="${CREDIS_TEST_PORT:-7911}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -x "$SERVER" ]; then
  echo "integration.sh: no server binary at '$SERVER'" >&2
  echo "usage: $0 /path/to/credis-server" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "integration.sh: python3 is required to drive the protocol checks" >&2
  exit 1
fi

LOG="$(mktemp)"
"$SERVER" --port "$PORT" --bind 127.0.0.1 >"$LOG" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -f "$LOG"
}
trap cleanup EXIT INT TERM

# Wait for the port to accept connections rather than sleeping a fixed amount.
i=0
until python3 -c "
import socket,sys
try:
    socket.create_connection(('127.0.0.1', $PORT), timeout=0.3).close()
except OSError:
    sys.exit(1)
" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -gt 100 ]; then
    echo "integration.sh: server did not start; log follows" >&2
    cat "$LOG" >&2
    exit 1
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "integration.sh: server exited during startup; log follows" >&2
    cat "$LOG" >&2
    exit 1
  fi
  sleep 0.1
done

python3 "$HERE/integration.py" "$PORT"
STATUS=$?

# The server must still be healthy after the whole run.
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "integration.sh: server died during the test run; log follows" >&2
  cat "$LOG" >&2
  exit 1
fi

exit "$STATUS"
