"use strict";
// Terminal-control leaf (JS-053): tty.raw / tty.cook / tty.size over abc/ANSI's
// ANSI* POSIX wrappers.  A real controlling tty is absent under ctest, so we
// drive a pty (tty.openpty) and set raw on the SLAVE fd.  Each assertion throws,
// which JABCRun turns into a non-zero exit (CTest fails) — repro-first.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

// equal-bytes over two Uint8Arrays
function bytesEq(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

const pty = tty.openpty();
if (typeof pty.master !== "number" || typeof pty.slave !== "number")
  fail("openpty did not return {master, slave} numbers");
if (pty.master < 0 || pty.slave < 0) fail("openpty returned a bad fd");

// 1. raw round-trips the original termios.  tty.raw returns the CURRENT
//    (cooked) termios as the saved state and switches the slave to raw.
const orig = tty.raw(pty.slave);
if (!(orig instanceof Uint8Array)) fail("tty.raw did not return a Uint8Array");
if (orig.length === 0) fail("tty.raw returned an empty saved buffer");

// Re-entering raw captures the NOW-current termios (the raw one) as its saved
// state — so `nowRaw` is the slave's raw termios bytes.  They must DIFFER from
// `orig`: raw mode cleared ECHO|ICANON|... and set VMIN/VTIME, i.e. the termios
// actually changed on the fd.
const nowRaw = tty.raw(pty.slave);
eq(nowRaw.length, orig.length, "termios size stable across raw");
if (bytesEq(orig, nowRaw)) fail("tty.raw did not change the termios (still cooked)");

// 2. cook restores: after cook(orig), the live termios must equal orig again.
tty.cook(pty.slave, orig);
const afterCook = tty.raw(pty.slave);          // captures the restored termios
if (!bytesEq(orig, afterCook))
  fail("tty.cook did not restore the original termios");
tty.cook(pty.slave, orig);                      // leave it cooked

// cook with a wrong-sized buffer is rejected (defensive size check)
{
  let threw = false;
  try { tty.cook(pty.slave, new Uint8Array(3)); } catch (e) { threw = true; }
  if (!threw) fail("tty.cook accepted a wrong-sized saved buffer");
}

// 3. winsize: stamp a size on the pty MASTER, read it back via tty.size on it.
//    (A pty propagates the size set on either end.)
tty.setSize(pty.master, 40, 132);
const sz = tty.size(pty.master);
eq(sz.rows, 40, "tty.size rows");
eq(sz.cols, 132, "tty.size cols");
// the slave sees the same size
const szs = tty.size(pty.slave);
eq(szs.rows, 40, "tty.size rows (slave)");
eq(szs.cols, 132, "tty.size cols (slave)");

// a fresh, distinct size overwrites (no accidental no-op)
tty.setSize(pty.slave, 24, 80);
const sz2 = tty.size(pty.master);
eq(sz2.rows, 24, "tty.size rows after resize");
eq(sz2.cols, 80, "tty.size cols after resize");

// tty.size on a non-tty fd throws (errno-mapped), like the io.* leaves.
{
  const f = io.open("/dev/null", "r");
  let threw = false;
  try { tty.size(f); } catch (e) { threw = true; }
  io.close(f);
  if (!threw) fail("tty.size(/dev/null) did not throw");
}

// tty.raw on a non-tty fd throws too.
{
  const f = io.open("/dev/null", "r");
  let threw = false;
  try { tty.raw(f); } catch (e) { threw = true; }
  io.close(f);
  if (!threw) fail("tty.raw(/dev/null) did not throw");
}

io.close(pty.slave);
io.close(pty.master);

io.log("tty.js OK");
