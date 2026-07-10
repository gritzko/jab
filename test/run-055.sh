#!/bin/sh
#  JAB e2e: a bare/relative `.js` ENTRY (`jab greet.js`) resolves via the upward
#  `./jsrc ../jsrc …` scan (`__runScript`, the require machine — NOT direct eval),
#  patches `process.argv[1]` to the resolved abspath so a script's `here` idiom
#  (self.slice(0, lastIndexOf("/"))) works, and forwards the tail args.  A bare
#  word WITHOUT `.js` is a VERB (routes to jsrc/main.js, the loop) — covered by
#  the loop tests, not here.  Usage: run-055.sh /path/to/jab

set -e

JAB="$1"
[ -n "$JAB" ] || { echo "usage: run-055.sh <jab>"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
#  macOS $TMPDIR sits behind the /var -> /private/var symlink; jab realpaths
#  the entry (Node patches argv[1] the same way), so expect the physical path.
WORK="$(cd "$WORK" && pwd -P)"

#  Layout: $WORK/jsrc/greet.js + $WORK/jsrc/lib/u.js ; run from $WORK/sub/deep.
mkdir -p "$WORK/jsrc/lib" "$WORK/sub/deep"

cat > "$WORK/jsrc/lib/u.js" <<'JS'
module.exports = { name: "u" };
JS

#  greet.js: bareword require of a sibling lib via the `here` idiom (so it
#  exercises the patched process.argv[1]), then prints its own abspath + arg.
cat > "$WORK/jsrc/greet.js" <<'JS'
const self = process.argv[1];
const here = self.slice(0, self.lastIndexOf("/"));
const u = require(here + "/lib/u.js");
io.log("self=" + self);
io.log("u=" + u.name);
io.log("args=" + JSON.stringify(args));
JS

#  1. bare `.js` `jab greet.js a` from a SUBDIR resolves $WORK/jsrc/greet.js via
#  the upward scan, patches process.argv[1] to the abspath, forwards the tail.
OUT="$WORK/out.txt"
( cd "$WORK/sub/deep" && "$JAB" greet.js a ) >/dev/null 2>"$OUT" \
  || { echo "FAIL: jab greet.js exited non-zero"; cat "$OUT"; exit 1; }
grep -Fqx "self=$WORK/jsrc/greet.js" "$OUT" || {
  echo "FAIL: process.argv[1] not patched to resolved abspath:"; cat "$OUT"; exit 1; }
grep -Fqx 'u=u' "$OUT" || {
  echo "FAIL: here-idiom require of jsrc/lib/u.js failed:"; cat "$OUT"; exit 1; }
grep -Fqx 'args=["a"]' "$OUT" || {
  echo "FAIL: tail args not forwarded:"; cat "$OUT"; exit 1; }

#  2. a missing `.js` bareword fails (no jsrc/<name>.js up to the ceiling).
if ( cd "$WORK/sub/deep" && "$JAB" nope.js ) >/dev/null 2>"$WORK/err.txt"; then
  echo "FAIL: jab nope.js should have failed"; cat "$WORK/err.txt"; exit 1
fi

echo "JAB requireJsrc OK"
