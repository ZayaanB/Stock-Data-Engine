#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 ITEM_COUNT COMMAND [ARGS...]" >&2
  exit 2
fi

item_count=$1
shift
if ! [[ $item_count =~ ^[1-9][0-9]*$ ]]; then
  echo "ITEM_COUNT must be a positive integer" >&2
  exit 2
fi

report=$(mktemp)
trap 'rm -f "$report"' EXIT
events=cycles,instructions,branches,branch-misses,cache-references,cache-misses

perf stat -x, -o "$report" -e "$events" -- "$@"
cat "$report"

awk -F, -v count="$item_count" '
  $3 == "cycles" { cycles=$1 }
  $3 == "instructions" { instructions=$1 }
  $3 == "branch-misses" { branch_misses=$1 }
  $3 == "cache-misses" { cache_misses=$1 }
  END {
    gsub(/ /, "", cycles)
    gsub(/ /, "", instructions)
    gsub(/ /, "", branch_misses)
    gsub(/ /, "", cache_misses)
    if (cycles ~ /^[0-9]+$/)
      printf "cycles/item: %.2f\n", cycles / count
    if (instructions ~ /^[0-9]+$/)
      printf "instructions/item: %.2f\n", instructions / count
    if (branch_misses ~ /^[0-9]+$/)
      printf "branch-misses/item: %.4f\n", branch_misses / count
    if (cache_misses ~ /^[0-9]+$/)
      printf "cache-misses/item: %.4f\n", cache_misses / count
  }
' "$report"
