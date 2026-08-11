"use strict";
// STATUS-020: the per-FILE .gitignore leaves — dog._igno_open / _match /
// _close over ONE named file.  No chain, no walk: the fold across levels
// and the .git/.be meta test stay in the JS matcher, so this exercises
// exactly one set — open, the tristate (incl. a negation and the dir/
// parent-prefix rule), an absent file, close and double-close.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const root = "/tmp/jabc-igno-test";
try { io.rmdir(root, true); } catch (e) {}
io.mkdir(root);
const gi = root + "/.gitignore";
{
  const fd = io.open(gi, "c");
  io._write(fd, utf8.Encode(
    "# a comment\n" +
    "*.o\n" +
    "!keep.o\n" +
    "build-*/\n" +
    "node_modules/\n" +
    "/root-only.txt\n"));
  io.close(fd);
}

// --- 1. open ------------------------------------------------------------
const h = dog._igno_open(gi);
if (!(h > 0)) fail("open handle: " + h);

// an absent .gitignore is the common case: 0, no exception.
eq(dog._igno_open(root + "/nowhere/.gitignore"), 0, "absent file -> 0");
eq(dog._igno_open(root + "/.gitignore.nope"), 0, "unreadable file -> 0");

// --- 2. the tristate ----------------------------------------------------
eq(dog._igno_match(h, "foo.o", false), 1, "*.o ignores foo.o");
eq(dog._igno_match(h, "a/b/foo.o", false), 1, "unanchored *.o matches deep");
eq(dog._igno_match(h, "core.c", false), -1, "no pattern matches core.c");
// last match wins: !keep.o negates the *.o above it -> 0, not -1.
eq(dog._igno_match(h, "keep.o", false), 0, "!keep.o un-ignores");
// dir-only patterns take is_dir into account.
eq(dog._igno_match(h, "build-debug", true), 1, "build-*/ ignores the dir");
eq(dog._igno_match(h, "build-debug", false), -1, "build-*/ skips a file");
// the dir-prefix rule: a `dir/` hit on ANY parent ignores everything below.
eq(dog._igno_match(h, "node_modules/x/index.js", false), 1, "parent dir/ hit");
eq(dog._igno_match(h, "a/node_modules/pkg/y.js", false), 1, "nested dir/ hit");
// anchored patterns pin to the set's own directory.
eq(dog._igno_match(h, "root-only.txt", false), 1, "/root-only.txt at root");
eq(dog._igno_match(h, "sub/root-only.txt", false), -1, "anchored: not nested");
// isDir defaults to false when omitted.
eq(dog._igno_match(h, "build-debug"), -1, "omitted isDir reads as a file");

// --- 3. close, idempotent ----------------------------------------------
eq(dog._igno_close(h), undefined, "close returns undefined");
eq(dog._igno_close(h), undefined, "double close is a no-op");
eq(dog._igno_close(0), undefined, "closing a 0 handle is a no-op");

// a fresh open after the closes still works (the box is recycled).
const h2 = dog._igno_open(gi);
if (!(h2 > 0)) fail("reopen handle: " + h2);
eq(dog._igno_match(h2, "thing.o", false), 1, "reopened set still matches");
dog._igno_close(h2);

// --- 4. no fd/slot wall (STATUS-020) ------------------------------------
// The map must hold NO descriptor: a pre-ABC-023 FILEMapRO ran the process
// out of fds at ~1017 opens and every later open silently reported 0 — i.e.
// "no .gitignore", i.e. nothing ignored, in a long-lived pager session.
// Past the old wall by 2x, with the decisions still right.
for (let i = 0; i < 2500; i++) {
  const hi = dog._igno_open(gi);
  if (!(hi > 0)) fail("open/close cycle " + i + " returned " + hi);
  if (dog._igno_match(hi, "foo.o", false) !== 1) fail("cycle " + i + " match");
  dog._igno_close(hi);
}
// and an absent path keeps answering 0 just as long (no fd burnt either).
for (let i = 0; i < 2500; i++) {
  const hz = dog._igno_open(root + "/nowhere/.gitignore");
  if (hz !== 0) fail("absent cycle " + i + " returned " + hz);
}

io.rmdir(root, true);
console.log("igno: OK");
