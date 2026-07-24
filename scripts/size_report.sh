#!/usr/bin/env bash
# size_report.sh <exe> <data-dir>  — prints distributable sizes; warns over 2GB.
set -euo pipefail

exe="$1"
data="${2:-}"

exe_bytes=$(stat -c%s "$exe")
data_bytes=0
if [ -n "$data" ] && [ -d "$data" ]; then
  data_bytes=$(du -sb "$data" | cut -f1)
fi
total=$((exe_bytes + data_bytes))

mb() { awk "BEGIN{printf \"%.2f\", $1/1048576}"; }

echo "-------------------- size report --------------------"
echo "  exe:   $(mb "$exe_bytes") MB  ($exe_bytes bytes)"
echo "  data:  $(mb "$data_bytes") MB"
echo "  total: $(mb "$total") MB  (ceiling: 2 GB)"
echo "------------------------------------------------------"

if [ "$total" -gt $((2048 * 1048576)) ]; then
  echo "WARNING: distributable exceeds the 2GB ceiling" >&2
fi
