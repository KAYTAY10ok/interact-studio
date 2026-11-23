#!/usr/bin/env bash
# Preserve the failing stage's diagnostic output on the Actions summary page.
set -euo pipefail
label=$1
shift
log=$(mktemp)
trap 'rm -f -- "$log"' EXIT
if "$@" > "$log" 2>&1; then
    cat "$log"
    exit 0
else
    result=$?
fi
cat "$log"
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    {
        printf '### %s failed (exit %s)\n\n' "$label" "$result"
        printf '\x60\x60\x60text\n'
        tail -n 65 "$log" | cut -c1-450
        printf '\x60\x60\x60\n'
    } >> "$GITHUB_STEP_SUMMARY"
fi
exit "$result"
