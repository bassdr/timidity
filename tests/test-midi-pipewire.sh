#!/bin/bash
# Test TiMidity++ PipeWire MIDI synthesis by sending MIDI events
# through the PipeWire graph to the TiMidity MIDI Sink.
#
# Requirements:
#   - TiMidity++ running with -ip (PipeWire MIDI interface)
#   - WirePlumber with MIDI session items patch
#   - pw-midiplay (from pipewire, USE=extra)
#
# Usage: ./test-midi-pipewire.sh [--file-only]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_MIDI="${SCRIPT_DIR}/test-scale.mid"

echo "=== TiMidity++ MIDI test suite ==="
echo

# --- Check prerequisites ---
echo "1. Checking prerequisites..."

if ! pgrep -x timidity >/dev/null 2>&1; then
    echo "   FAIL: timidity is not running"
    echo "   Start it with: rc-service timidity start"
    exit 1
fi
echo "   OK: timidity is running ($(pgrep -a timidity | head -1 | grep -oP '(?<=timidity ).*'))"

# Find TiMidity's MIDI sink serial number
TIMIDITY_SERIAL=$(pw-dump 2>/dev/null | python3 -c "
import json, sys
data = json.load(sys.stdin)
for obj in data:
    props = obj.get('info', {}).get('props', {})
    if props.get('media.class') == 'Midi/Sink' and 'TiMidity' in props.get('node.name', ''):
        print(props.get('object.serial', ''))
        break
" 2>/dev/null)

if [ -n "$TIMIDITY_SERIAL" ]; then
    echo "   OK: TiMidity PipeWire MIDI Sink found (serial=$TIMIDITY_SERIAL)"
else
    echo "   WARN: TiMidity PipeWire MIDI Sink not found (is -ip mode active?)"
fi

# Check audio output connection
if pw-link -l 2>/dev/null | grep -q "TiMidity.*output"; then
    echo "   OK: TiMidity audio output connected to PipeWire"
else
    echo "   WARN: TiMidity audio output not connected"
fi
echo

# --- Test 1: File playback ---
echo "2. Testing file playback (C major scale via timidity CLI)..."
if [ -f "$TEST_MIDI" ]; then
    echo "   Playing ${TEST_MIDI}..."
    echo "   You should hear a C major scale through your headset."
    timidity "$TEST_MIDI" 2>&1 | sed 's/^/   /'
    echo "   Done."
else
    echo "   SKIP: test-scale.mid not found"
fi
echo

if [ "${1:-}" = "--file-only" ]; then
    echo "=== File-only mode, skipping PipeWire MIDI test ==="
    exit 0
fi

# --- Test 2: PipeWire MIDI (send notes to the TiMidity sink) ---
echo "3. Testing PipeWire MIDI input (pw-midiplay -> TiMidity MIDI Sink)..."

if ! command -v pw-midiplay >/dev/null 2>&1; then
    echo "   SKIP: pw-midiplay not found (emerge pipewire with USE=extra)"
    exit 0
fi

if [ -z "$TIMIDITY_SERIAL" ]; then
    echo "   SKIP: No TiMidity MIDI Sink found"
    exit 0
fi

echo "   Sending C major scale via pw-midiplay --target=$TIMIDITY_SERIAL..."
echo "   You should hear the same scale again."
pw-midiplay --target="$TIMIDITY_SERIAL" "$TEST_MIDI" 2>&1 | sed 's/^/   /'
echo "   Done."

echo
echo "=== Tests complete ==="
