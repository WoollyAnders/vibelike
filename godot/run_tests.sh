#!/usr/bin/env bash
# Godot-side boundary gate: run the GDExtension smoke tests headless.
#   GODOT=/path/to/godot ./run_tests.sh        (defaults to `godot` on PATH)
# The full 13-test suite runs via the SDL build (./build.sh win-debug + ./dustfall.exe --<test>)
# until G3 retires SDL; these prove the sim behaves identically ACROSS the GDExtension boundary.
set -euo pipefail
cd "$(dirname "$0")"
GODOT="${GODOT:-godot}"
"$GODOT" --headless --path . --import >/dev/null 2>&1 || true
fail=0
for t in determinism play_check smoke; do
  out=$("$GODOT" --headless --path . --script "res://tests/$t.gd" 2>&1 | grep -iE "^$t|determinism|play_check|smoke" | tail -1 || true)
  echo "$out"
  echo "$out" | grep -qiE "PASS|det=true|kb_moved=true" || { echo "  -> $t FAILED"; fail=1; }
done
[ $fail = 0 ] && echo "GODOT GATE: ALL PASS" || { echo "GODOT GATE: FAILURES"; exit 1; }
