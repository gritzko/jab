"use strict";
// JABC ULOGOpen/AppendAt/Close bindings (approach-(a) prep): the native
// file-backed ULOG (booked mmap + wh128 sidecar) driven through the
// abc._ulog_open / _append / _count / _rowUri / _rowTime / _close leaves.
// Proves the bindings are not dead-on-arrival:
//   1. open -> append (ts-preserving, monotonic) -> close -> reopen round-trip;
//   2. zero-tail recovery through the binding (un-trimmed page pad -> IDLE).
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const path = "/tmp/jabc-ulog-book.log";
const idxp = "/tmp/.jabc-ulog-book.log.idx";
function cleanup() { try { io.unlink(path); } catch (e) {} try { io.unlink(idxp); } catch (e) {} }
cleanup();

// --- 1. open -> append -> close -> reopen round-trip ---------------------
let h = abc._ulog_open(path);
if (!(h > 0)) fail("open handle");
// ts-preserving: pass explicit monotonic stamps, read them back verbatim.
const t0 = abc._ulog_append(h, 100n, "post", "?/dogs/main#a");
const t1 = abc._ulog_append(h, 200n, "get", "//host/repo");
const t2 = abc._ulog_append(h, 300n, "post", "?/dogs/main#b");
eq(t0, 100n, "ts preserved row0");
eq(t1, 200n, "ts preserved row1");
eq(t2, 300n, "ts preserved row2");
eq(abc._ulog_count(h), 3, "count after append");
if (!(t0 < t1 && t1 < t2)) fail("monotonic ts");

// monotonicity guard: a stale ts must be refused (throws ULOGCLOCK).
let clocked = false;
try { abc._ulog_append(h, 300n, "post", "?x"); } catch (e) { clocked = true; }
if (!clocked) fail("stale ts must be refused (ULOGCLOCK)");
eq(abc._ulog_count(h), 3, "count unchanged after refused append");

abc._ulog_close(h);

// reopen: all three rows survive the close/reopen with fields intact.
h = abc._ulog_open(path);
eq(abc._ulog_count(h), 3, "count after reopen");
eq(abc._ulog_rowUri(h, 0), "?/dogs/main#a", "uri row0");
eq(abc._ulog_rowUri(h, 1), "//host/repo", "uri row1");
eq(abc._ulog_rowUri(h, 2), "?/dogs/main#b", "uri row2");
eq(abc._ulog_rowTime(h, 0), 100n, "ts row0 after reopen");
eq(abc._ulog_rowTime(h, 2), 300n, "ts row2 after reopen");
// append past the reopen tail (monotonic continues).
abc._ulog_append(h, 400n, "post", "?/dogs/main#c");
eq(abc._ulog_count(h), 4, "count after post-reopen append");
abc._ulog_close(h);

// --- 2. zero-tail recovery through the binding ---------------------------
// Simulate an un-trimmed abnormal exit: grow the on-disk file with a page of
// trailing NULs below the last row, drop the sidecar, then reopen.  The
// binding's ULOGOpen back-scans the pad: DATA = last complete row, pad ->
// IDLE.  All four rows read back, and the close trims the pad away.
const sz = io.stat(path).size;
io._truncate(path, sz + 4096);       // ftruncate up: page-aligned NUL pad
io.unlink(idxp);                     // force the rebuild path
const before = io.stat(path).size;
if (!(before === sz + 4096)) fail("pad not applied");

h = abc._ulog_open(path);            // must back-scan the pad, not choke on it
eq(abc._ulog_count(h), 4, "count after zero-tail reopen");
eq(abc._ulog_rowTime(h, 3), 400n, "tail ts after zero-tail reopen");
abc._ulog_close(h);

const after = io.stat(path).size;
if (!(after < before)) fail("close must trim the NUL pad (" + after + " !< " + before + ")");

cleanup();
console.log("ulog_book: OK");
