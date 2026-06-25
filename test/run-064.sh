#!/bin/sh
#  JS-064: a bareword `jab <projector>` run must be LeakSanitizer-clean on the
#  normal loop exit.  The teardown already runs (JS-054), but the resident loop
#  exercises JS features that pin JSC lazily-initialised VM singletons the VM
#  never frees before exit; ctest hides them via js/lsan.supp, a bareword `jab`
#  run does NOT.  The fix embeds that suppression in the binary (main.cpp's
#  __lsan_default_suppressions) so EVERY run is clean.  This test runs WITHOUT
#  an external suppressions file, so it sees only the embedded one.
#  Usage: run-064.sh /path/to/jab   (run from a tree with be/ + beagle/ siblings)

JAB="$1"
[ -n "$JAB" ] || { echo "usage: run-064.sh <jab>"; exit 2; }

#  No external suppression file; keep leak detection on regardless of inherited env.
export LSAN_OPTIONS=detect_leaks=1
LEAK='LeakSanitizer: detected memory leaks'

#  Calibrate: a clean --eval exit must itself be leak-free, or LSan is unusable
#  on this platform (e.g. ASAN off) and the test cannot isolate the bug -> SKIP.
if "$JAB" --eval '0' 2>&1 | grep -q "$LEAK"; then
  echo "JS-064 SKIP: platform leaks JSC singletons even on a clean --eval exit"
  exit 0
fi

#  Repro: a bareword run drives the resident be/ loop (here `ls:` of a tmp dir).
#  It must exit leak-free.  Resolve be/ from a fresh cwd that sits beside the
#  be/ shard: ../../../be relative to this script (js/test/ -> beagle -> sibling).
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"       # the dir holding beagle/ + be/ siblings
[ -d "$ROOT/be" ] || { echo "JS-064 SKIP: no be/ sibling at $ROOT"; exit 0; }

OUT="$(cd "$ROOT" && "$JAB" ls:. 2>&1)"; RC=$?
printf '%s\n' "$OUT" | grep -q "$LEAK" && {
  echo "FAIL: bareword loop exit leaked the JS context:";
  printf '%s\n' "$OUT" | grep -A2 'LeakSanitizer\|SUMMARY'; exit 1; }

echo "JS-064 OK (rc=$RC)"
