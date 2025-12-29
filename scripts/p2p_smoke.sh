#!/usr/bin/env bash
set -euo pipefail

# Runs a 2-peer simplep2p/gossipsub smoke using phosphor_mp_lab in two processes.
#
# This is intentionally not part of `make test` because it depends on real libp2p
# behavior + networking environment (DHT bootstrap peers, UDP buffers, etc.).

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

suffix="$(date +%s)-$$-${RANDOM}"
topic="phos.room.smoke.${suffix}"
discovery="phos.smoke.${suffix}"
seconds="${1:-8}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

logA="$tmpdir/a.log"
logB="$tmpdir/b.log"

echo "[p2p-smoke] topic=${topic} discovery=${discovery} seconds=${seconds}"

./phosphor_mp_lab --mode p2p --topic "$topic" --discovery "$discovery" --name smokeA --host --seconds "$seconds" >"$logA" 2>&1 &
pidA=$!
./phosphor_mp_lab --mode p2p --topic "$topic" --discovery "$discovery" --name smokeB --seconds "$seconds" >"$logB" 2>&1 &
pidB=$!

wait "$pidA" || true
wait "$pidB" || true

hashA="$(grep -Eo 'save_blake3=[0-9a-f]+' "$logA" | tail -n 1 | cut -d= -f2 || true)"
hashB="$(grep -Eo 'save_blake3=[0-9a-f]+' "$logB" | tail -n 1 | cut -d= -f2 || true)"
peersA="$(grep -Eo 'peers=[0-9]+' "$logA" | tail -n 1 | cut -d= -f2 || true)"
peersB="$(grep -Eo 'peers=[0-9]+' "$logB" | tail -n 1 | cut -d= -f2 || true)"

echo "[p2p-smoke] peersA=${peersA:-?} peersB=${peersB:-?} hashA=${hashA:-?} hashB=${hashB:-?}"

if [[ -z "$hashA" || -z "$hashB" ]]; then
  echo "[p2p-smoke] FAIL: missing save_blake3 in logs (simplep2p likely crashed or never initialized properly)." >&2
  echo "--- smokeA log ---" >&2
  tail -n 200 "$logA" >&2 || true
  echo "--- smokeB log ---" >&2
  tail -n 200 "$logB" >&2 || true
  exit 1
fi

if [[ "$hashA" != "$hashB" ]]; then
  echo "[p2p-smoke] FAIL: docs did not converge (hash mismatch)." >&2
  exit 1
fi

if [[ "${peersA:-0}" -lt 1 || "${peersB:-0}" -lt 1 ]]; then
  echo "[p2p-smoke] FAIL: peers were not discovered/handshaked (peersA/peersB < 1)." >&2
  exit 1
fi

echo "[p2p-smoke] PASS"


