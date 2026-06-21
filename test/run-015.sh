#!/bin/sh
#  JS-015 e2e: the argv tail (tokens after the script path) must reach JS as the
#  global `args`, and `process.argv` must be Node-shaped: ["jabc", script, ...tail].
#  io.log writes to stderr, so we capture stderr and grep the JSON it prints.
#  Usage: run-015.sh /path/to/jabc

set -e

JABC="$1"
[ -n "$JABC" ] || { echo "usage: run-015.sh <jabc>"; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

SCRIPT="$WORK/argv.js"
cat > "$SCRIPT" <<'JS'
io.log("args=" + JSON.stringify(args));
io.log("argv=" + JSON.stringify(process.argv));
JS

OUT="$WORK/out.txt"
#  Pass a tail of three tokens after the script path.
"$JABC" "$SCRIPT" a b c > /dev/null 2>"$OUT" || { echo "jabc exited non-zero"; cat "$OUT"; exit 1; }

#  -F: the JSON brackets/quotes are literal, not a regex.
grep -Fqx 'args=["a","b","c"]' "$OUT" || {
  echo "FAIL: expected args=[\"a\",\"b\",\"c\"], got:"; cat "$OUT"; exit 1; }

#  process.argv = ["jabc", <script path>, "a", "b", "c"].
grep -Fqx "argv=[\"jabc\",\"$SCRIPT\",\"a\",\"b\",\"c\"]" "$OUT" || {
  echo "FAIL: process.argv mismatch, got:"; grep '^argv=' "$OUT"; exit 1; }

#  --eval with no script file: args is the empty tail (tokens after --eval CODE).
EOUT="$WORK/eval.txt"
"$JABC" --eval 'io.log("eargs=" + JSON.stringify(args))' > /dev/null 2>"$EOUT" || {
  echo "jabc --eval exited non-zero"; cat "$EOUT"; exit 1; }
grep -Fqx 'eargs=[]' "$EOUT" || {
  echo "FAIL: expected eargs=[] under --eval, got:"; cat "$EOUT"; exit 1; }

echo "JS-015 OK"
