#!/bin/sh
#  JAB-001 e2e: `jab <bareword>` and `require("<bareword>")` resolve via the
#  upward `./be ../be …` scan (the require machine, not direct eval).  A bare
#  name runs the require machine; `process.argv[1]` is patched to the resolved
#  abspath so a script's `here` idiom (self.slice(0, lastIndexOf("/"))) works.
#  Usage: run-055.sh /path/to/jab

set -e

JAB="$1"
[ -n "$JAB" ] || { echo "usage: run-055.sh <jab>"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

#  Layout: $WORK/be/greet.js + $WORK/be/lib/u.js ; run from $WORK/sub/deep.
mkdir -p "$WORK/be/lib" "$WORK/sub/deep"

cat > "$WORK/be/lib/u.js" <<'JS'
module.exports = { name: "u" };
JS

#  greet.js: bareword require of a sibling lib via the `here` idiom (so it
#  exercises the patched process.argv[1]), then prints its own abspath + arg.
cat > "$WORK/be/greet.js" <<'JS'
const self = process.argv[1];
const here = self.slice(0, self.lastIndexOf("/"));
const u = require(here + "/lib/u.js");
io.log("self=" + self);
io.log("u=" + u.name);
io.log("args=" + JSON.stringify(args));
JS

#  1. bareword `jab greet a` from a SUBDIR resolves $WORK/be/greet.js via the
#  upward scan, patches process.argv[1] to the abspath, forwards the tail.
OUT="$WORK/out.txt"
( cd "$WORK/sub/deep" && "$JAB" greet a ) >/dev/null 2>"$OUT" \
  || { echo "FAIL: jab greet exited non-zero"; cat "$OUT"; exit 1; }
grep -Fqx "self=$WORK/be/greet.js" "$OUT" || {
  echo "FAIL: process.argv[1] not patched to resolved abspath:"; cat "$OUT"; exit 1; }
grep -Fqx 'u=u' "$OUT" || {
  echo "FAIL: here-idiom require of be/lib/u.js failed:"; cat "$OUT"; exit 1; }
grep -Fqx 'args=["a"]' "$OUT" || {
  echo "FAIL: tail args not forwarded:"; cat "$OUT"; exit 1; }

#  2. bareword `jab greet.js` (with the .js) resolves the same file.
OUT2="$WORK/out2.txt"
( cd "$WORK/sub" && "$JAB" greet.js ) >/dev/null 2>"$OUT2" \
  || { echo "FAIL: jab greet.js exited non-zero"; cat "$OUT2"; exit 1; }
grep -Fqx "self=$WORK/be/greet.js" "$OUT2" || {
  echo "FAIL: <name>.js form did not resolve:"; cat "$OUT2"; exit 1; }

#  3. a missing bareword fails (no be/<name>[.js] up to the ceiling).
if ( cd "$WORK/sub/deep" && "$JAB" nope ) >/dev/null 2>"$WORK/err.txt"; then
  echo "FAIL: jab nope should have failed"; cat "$WORK/err.txt"; exit 1
fi

echo "JAB-001 requireBe OK"
