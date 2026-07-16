"use strict";
// JAB-008: the BASS ownership contract at the JS/C boundary — a binding
// invocation leaks NO ABC_BASS scratch.  Pre-fix, every fold permanently
// leaked its carve set (~4.3x the blob size), draining the 1GB arena;
// post-fix the same loop survives.  Sized for the 10s test timeout: a
// 256KB blob leaks ~1.1MB/fold pre-fix, so the arena exhausts at ~960
// of the 1500 iterations; post-fix the loop runs in ~7s.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + JSON.stringify(a) + " !== " + JSON.stringify(b)); }
const E = (s) => utf8.Encode(s);

// --- 256KB fold x1500 in ONE process: each from-blob fold carves BASS
// scratch several times the blob size; without the per-invocation rewind
// the 1GB arena drains at ~iter 960 and every later BASS op fails too.
{
  let line = "";
  for (let i = 0; i < 63; i++) line += "x";
  line += "\n";
  let src = "";
  for (let i = 0; i < (1 << 18) / 64; i++) src += line;
  const blob = E(src);
  const w = abc.ram("WEAVE", 16 * (1 << 20));
  for (let i = 0; i < 1500; i++) {
    try { w.fold(null, blob, "txt", "00000000005774ed"); }
    catch (e) { fail("BASS leak: fold died at iteration " + i + " (" + e + ")"); }
  }
  eq(w.size > 0, true, "folded weave has tokens");
}

// --- merge variant: fork/merge in a loop leaks nothing either (WEAVEMerge
// carves union scratch per call; a cheap 3-line weave x500 catches a rewind
// regression on the merge path without dominating the suite's runtime).
{
  const C1 = "0000000000000001", C2 = "0000000000000002", C3 = "0000000000000003";
  for (let i = 0; i < 500; i++) {
    const w0 = abc.ram("WEAVE", 1 << 16); w0.fold(null, E("a\nb\nc\n"), "c", C1);
    const wa = abc.ram("WEAVE", 1 << 16); wa.fold(w0, E("A\nb\nc\n"), "c", C2);
    const wb = abc.ram("WEAVE", 1 << 16); wb.fold(w0, E("a\nb\nC\n"), "c", C3);
    const m = abc.ram("WEAVE", 1 << 16);
    try { m.merge(wa, wb, C3); }
    catch (e) { fail("BASS leak: merge died at iteration " + i + " (" + e + ")"); }
  }
}
