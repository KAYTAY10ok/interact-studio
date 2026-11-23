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
if [[ ${GITHUB_ACTIONS:-} == true ]]; then
    diagnostic=$(tail -n 35 "$log" | cut -c1-320)
    diagnostic=${diagnostic//%/%25}
    diagnostic=${diagnostic//$'\r'/%0D}
    diagnostic=${diagnostic//$'\n'/%0A}
    printf '::error title=%s failed::%s\n' "$label" "$diagnostic"
fi
if [[ -n ${GITHUB_STEP_SUMMARY:-} ]]; then
    summary=$GITHUB_STEP_SUMMARY
    if command -v cygpath >/dev/null; then summary=$(cygpath -u "$summary"); fi
    {
        printf '### %s failed (exit %s)\n\n' "$label" "$result"
        printf '\x60\x60\x60text\n'
        tail -n 65 "$log" | cut -c1-450
        printf '\x60\x60\x60\n'
    } >> "$summary"
fi
exit "$result"
