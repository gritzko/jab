#!/bin/sh
#  JS-054: jabc's script-file error exits must run the same teardown as the
#  success path (JSClose etc.), so a failed open does not leak the whole JS
#  context.  We detect the leak WITHOUT the broad JSC suppression (which would
#  mask it); if the platform leaks JSC singletons even on a clean exit, the
#  signal is unusable, so we self-calibrate against the success path and SKIP.
#  Usage: run-054.sh /path/to/jabc

JABC="$1"
[ -n "$JABC" ] || { echo "usage: run-054.sh <jabc>"; exit 2; }

#  No broad suppression; keep leak detection on regardless of the inherited env.
export LSAN_OPTIONS=detect_leaks=1
LEAK='LeakSanitizer: detected memory leaks'

#  Calibrate: a clean exit must itself be leak-free or the test cannot isolate
#  the bug (some platforms never free JSC singletons).
if "$JABC" --eval '0' 2>&1 | grep -q "$LEAK"; then
  echo "JS-054 SKIP: platform leaks JSC singletons on a clean exit"
  exit 0
fi

#  Repro: opening a missing script file must free the context (no leak) and
#  still print the error + exit non-zero.  The path MUST end in `.js` — a
#  non-.js first arg now routes to be/main.js (the loop), not a direct open.
OUT="$("$JABC" /no/such/jabc/file.js 2>&1)"; RC=$?
printf '%s\n' "$OUT" | grep -Fq 'Error: cannot open /no/such/jabc/file.js' || {
  echo "FAIL: missing-file error message changed:"; printf '%s\n' "$OUT"; exit 1; }
[ "$RC" -ne 0 ] || { echo "FAIL: missing-file run returned 0"; exit 1; }
printf '%s\n' "$OUT" | grep -q "$LEAK" && {
  echo "FAIL: missing-file exit leaked the JS context:"; printf '%s\n' "$OUT"; exit 1; }

echo "JS-054 OK"
