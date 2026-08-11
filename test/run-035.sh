#!/bin/sh
#  JAB-035 e2e: a jab built with -DJAB_JSRC carries a default jsrc pack and
#  appends it as the LAST entry of the frozen jsrc stack — the floor.  With no
#  jsrc/ anywhere a bareword must still resolve (out of
#  <cache>/jsrcs/<contenthash>/, extracted once), and a user jsrc/ must
#  override the floor FILE BY FILE.  `--nofloor` asserts the opposite for a
#  build without the option: no pack, no cache dir, the old "cannot find" death.
#  Usage: run-035.sh /path/to/jab [--nofloor]

set -e

JAB="$1"
[ -n "$JAB" ] || { echo "usage: run-035.sh <jab> [--nofloor]"; exit 2; }
MODE="$2"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
WORK="$(cd "$WORK" && pwd -P)"

#  A scratch HOME is both the climb CEILING (so there is provably no jsrc/
#  anywhere) and the cache root — the real ~/.cache is never touched.
HOME="$WORK/home"
export HOME
unset XDG_CACHE_HOME
mkdir -p "$HOME/work"
CACHE="$HOME/.cache/jsrcs"

#  resolve <spec> — print "R=<abspath>" (io.log goes to stderr).
resolve() {
  ( cd "$HOME/work" && "$JAB" --eval "io.log('R=' + require.resolve('$1'))" ) 2>&1
}

if [ "$MODE" = "--nofloor" ]; then
  OUT="$(resolve main.js || true)"
  case "$OUT" in
    *"cannot find 'jsrc/main.js'"*) ;;
    *) echo "FAIL: an unpacked jab resolved a bareword: $OUT"; exit 1;;
  esac
  [ ! -d "$CACHE" ] || { echo "FAIL: an unpacked jab created $CACHE"; exit 1; }
  echo "JAB jsrcNoFloor OK"
  exit 0
fi

#  1. no jsrc anywhere: the bareword resolves out of the extracted pack.
OUT="$(resolve main.js)" || { echo "FAIL: bareword resolve died: $OUT"; exit 1; }
ABS="${OUT#R=}"
case "$ABS" in
  "$CACHE"/*/main.js) ;;
  *) echo "FAIL: not resolved from the pack cache: $OUT"; exit 1;;
esac
HASH="$(dirname "$ABS")"
[ -f "$HASH/main.js" ] || { echo "FAIL: $HASH/main.js absent"; exit 1; }
[ "$(basename "$HASH")" != "jsrc" ] || { echo "FAIL: the cache leaf is jsrc/"; exit 1; }

#  1b. EVERY packed file is reachable as a bareword (one process: deep and
#  >100-char paths, i.e. the ustar prefix split, come back whole).
( cd "$HOME/work" && "$JAB" --eval "
  const d = '$HASH';
  for (const p of io.readdir(d, {recursive: true}))
    if (p.slice(-3) === '.js' && require.resolve(p) !== d + '/' + p)
      throw 'floor miss: ' + p;
  io.log('all packed files resolve')" ) >/dev/null 2>"$WORK/all.txt" || {
  echo "FAIL: not every packed file resolves:"; cat "$WORK/all.txt"; exit 1; }

#  2. a second run must REUSE the extraction, not redo it: a marker written
#  into the cached file survives (a re-extract would overwrite the bytes).
echo "// JAB-035 marker" >> "$HASH/main.js"
OUT2="$(resolve main.js)" || { echo "FAIL: second resolve died: $OUT2"; exit 1; }
[ "$OUT2" = "$OUT" ] || { echo "FAIL: second run moved: $OUT2"; exit 1; }
grep -q "JAB-035 marker" "$HASH/main.js" || {
  echo "FAIL: the pack was extracted again over a present cache dir"; exit 1; }
if ls -d "$CACHE"/.tmp-* >/dev/null 2>&1; then
  echo "FAIL: a temp extraction dir was left behind"; exit 1
fi

#  3. overlay: a user jsrc/ holding ONE file wins for THAT file; its siblings
#  still come from the floor (per-file fallthrough over the stack).
SIB="$(ls "$HASH" | grep '\.js$' | grep -v '^main\.js$' | head -1)"
[ -n "$SIB" ] || { echo "FAIL: the pack has no sibling of main.js to test"; exit 1; }
mkdir -p "$HOME/work/jsrc"
echo "// mine" > "$HOME/work/jsrc/main.js"
OUT3="$(resolve main.js)" || { echo "FAIL: overlay resolve died: $OUT3"; exit 1; }
[ "$OUT3" = "R=$HOME/work/jsrc/main.js" ] || {
  echo "FAIL: the user jsrc/main.js did not win: $OUT3"; exit 1; }
OUT4="$(resolve "$SIB")" || { echo "FAIL: sibling resolve died: $OUT4"; exit 1; }
[ "$OUT4" = "R=$HASH/$SIB" ] || {
  echo "FAIL: $SIB did not fall through to the floor: $OUT4"; exit 1; }

echo "JAB jsrcFloor OK"
