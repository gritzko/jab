"use strict";
// CFOLD family (DIS-082): one file's whole DAG history as a 'V' TLV blob in a
// u8 buffer — the APPEND-ONLY weave.  fold = CFOLDFold (diff a revision in
// against its ANCESTOR closure), merge = CFOLDMerge (no content of its own),
// alive/produce emit bytes at the tip / any rev, and rewind(rev)/next() is the
// per-token iteration cursor (BODY offsets, blame-composable).  Mirrors
// dog/test/CFOLD01-03.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + JSON.stringify(a) + " !== " + JSON.stringify(b)); }
const dec = (u8) => utf8.Decode(u8);
const E = (s) => utf8.Encode(s);
const C1 = "0000000000000001", C2 = "0000000000000002", C3 = "0000000000000003", C4 = "0000000000000004";

function alive(w) { const o = io.buf(1 << 16); w.alive(o); return dec(o.data()); }
function prod(w, rev) { const o = io.buf(1 << 16); w.produce(rev, o); return dec(o.data()); }
function threw(f) { try { f(); return false; } catch (e) { return true; } }

// --- from-blob round-trip: CFOLDAlive of a from-blob weave reproduces the blob.
{
  const cases = ["", "abc", "int x = 1;\n", "int x = 1;", "a\nb\nc\n",
                 "int main(void){\n    return 0;\n}\n", "\n\n\nx\n"];
  for (const src of cases) {
    const w = abc.ram("CFOLD", 1 << 18);
    w.fold(null, E(src), "c", "00000000c0ffee01", []);
    eq(alive(w), src, "roundtrip " + JSON.stringify(src));
    eq(w.empty(), src.length === 0, "empty " + JSON.stringify(src));
  }
}

// --- a from-blob weave is one commit; every token is its insert.  The
// rewind(rev)/next() cursor is the iteration API: tok.off/.end are BODY
// offsets (the identity — blame(off) names the inserter), tok.text a view.
{
  const w = abc.ram("CFOLD", 1 << 16);
  w.fold(null, E("a\nb\nc\n"), "c", C1, []);
  const cs = w.commits;
  eq(cs.length, 1, "from-blob has 1 commit");
  eq(cs[0], C1, "commits[0] is the folded hashlet");
  eq(cs[0].length, 16, "commit is a 16-char hashlet");

  w.rewind(C1);
  let n = 0, produced = "";
  while (w.next()) {
    const t = w.tok;
    if (t.end <= t.off) fail("step: empty token at " + n);
    eq(dec(t.text).length, t.end - t.off, "step: text spans [off,end)");
    eq(w.blame(t.off), C1, "step: from-blob token " + n + " blames to C1");
    if (t.alive) produced += dec(t.text);
    n++;
  }
  if (n <= 0) fail("step: from-blob has no tokens");
  eq(produced, "a\nb\nc\n", "step: alive concatenation is the produced text");
}

// --- diff fold: fold v2 onto v1 with ancestors [C1].  tip == v2, and EVERY
// rev produces back byte-exact.
{
  const w = abc.ram("CFOLD", 1 << 16);
  w.fold(null, E("a\nb\nc\n"), "c", C1, []);
  const w2 = abc.ram("CFOLD", 1 << 16);
  w2.fold(w, E("a\nB\nc\n"), "c", C2, [C1]);
  eq(alive(w2), "a\nB\nc\n", "diff tip");
  eq(prod(w2, C1), "a\nb\nc\n", "produce rev C1");
  eq(prod(w2, C2), "a\nB\nc\n", "produce rev C2");
  eq(w2.commits.length, 2, "folded weave has 2 commits");
  eq(w2.commits[1], C2, "commits are in build order");
  if (!threw(() => prod(w2, "00000000deadbeef"))) fail("produce of an unknown rev must throw");

  // BLAME: identity IS the body offset.  The body is append-only, so the first
  // commit owns [0, 6) and the second's insert starts exactly at 6.
  eq(w2.blame(0), C1, "blame: body offset 0 is the first commit");
  eq(w2.blame("a\nb\nc\n".length), C2, "blame: the appended range is the second commit");

  // at C2 the edited token blames to C2, the rest to C1; the tombed old
  // token is still WALKED (alive=false), never rendered.
  w2.rewind(C2);
  let fromC2 = 0, txt2 = "", tombed = 0;
  while (w2.next()) {
    const t = w2.tok;
    if (!t.alive) { tombed++; continue; }
    if (w2.blame(t.off) === C2) fromC2++;
    txt2 += dec(t.text);
  }
  eq(fromC2, 1, "step at C2: exactly one alive token comes from C2");
  eq(tombed, 1, "step at C2: the replaced token is walked as a tomb");
  eq(txt2, "a\nB\nc\n", "step at C2: alive concatenation is the produced text");
  w2.rewind(C1);
  let txt1 = "";
  while (w2.next()) {
    const t = w2.tok;
    if (!t.alive) fail("step at C1: nothing may be tombed yet");
    eq(w2.blame(t.off), C1, "step at C1: nothing from C2 is visible");
    txt1 += dec(t.text);
  }
  eq(txt1, "a\nb\nc\n", "step at C1: alive concatenation is rev C1");
}

// --- emit a diff as HUNK records: line 4 d->X, from C1 to C2.  The emitted
// hunk reads/renders through the existing HUNK cursor (mirrors
// dog/test/CFOLD05.c, which holds byte-parity with the retired WEAVE emitters).
{
  const w = abc.ram("CFOLD", 1 << 16);
  w.fold(null, E("a\nb\nc\nd\ne\nf\ng\n"), "c", C1, []);
  const w2 = abc.ram("CFOLD", 1 << 16);
  w2.fold(w, E("a\nb\nc\nX\ne\nf\ng\n"), "c", C2, [C1]);

  const hd = abc.ram("HUNK", 1 << 16);
  w2.emitDiff(C1, C2, "foo.c", "deadbeef", hd);
  let n = 0;
  hd.rewind();
  while (hd.next()) {
    n++;
    eq(dec(hd.uri), "diff:foo.c?deadbeef#L1", "diff hunk uri");
    eq(dec(hd.text), "a\nb\nc\nXd\ne\nf\ng\n", "diff hunk body interleaves +/- tokens");
    if (hd.toks.length !== 15) fail("diff hunk token count: " + hd.toks.length);
    const out = io.buf(1 << 14); hd.plain(out);
    if (dec(out.data()).indexOf("foo.c") < 0) fail("diff render missing uri");
  }
  eq(n, 1, "single-window diff is one hunk");

  // emitFull with the "diff:" scheme yields the same body+uri for this window.
  const hf = abc.ram("HUNK", 1 << 16);
  w2.emitFull(C1, C2, "foo.c", "diff:", "deadbeef", hf);
  let m = 0;
  hf.rewind();
  while (hf.next()) { m++; eq(dec(hf.text), "a\nb\nc\nXd\ne\nf\ng\n", "full-diff body"); }
  eq(m, 1, "single-window full-diff is one hunk");
  if (!threw(() => w2.emitDiff(C1, "00000000deadbeef", "foo.c", "", hf)))
    fail("emitDiff of an unknown rev must throw");
}

// --- ONE weave, no weave pair: base C1, then two CONCURRENT folds (each with
// ancestors [C1], so each ignores the other), then a merge that carries no
// content.  Its produce is the RGA-merged text (DIS-080: markerless).
{
  const w = abc.ram("CFOLD", 1 << 16);
  w.fold(null, E("a\nb\nc\n"), "c", C1, []);
  const wo = abc.ram("CFOLD", 1 << 16);
  wo.fold(w, E("A\nb\nc\n"), "c", C2, [C1]);          // ours: line 1
  const wt = abc.ram("CFOLD", 1 << 16);
  wt.fold(wo, E("a\nb\nC\n"), "c", C3, [C1]);         // theirs: line 3
  eq(prod(wt, C2), "A\nb\nc\n", "branch C2 still produces its own view");
  eq(prod(wt, C3), "a\nb\nC\n", "branch C3 ignores C2");

  const wm = abc.ram("CFOLD", 1 << 16);
  wm.merge(wt, C4, [C1, C2, C3]);
  eq(wm.commits.length, 4, "the merge appends one commit record");
  eq(alive(wm), "A\nb\nC\n", "merge tip combines both branches");
  eq(prod(wm, C4), "A\nb\nC\n", "the merge produces the RGA-merged text");
  eq(prod(wm, C1), "a\nb\nc\n", "the base rev survives the merge");
  eq(prod(wm, C2), "A\nb\nc\n", "branch C2 survives the merge");
  eq(prod(wm, C3), "a\nb\nC\n", "branch C3 survives the merge");
}

// --- conflicting merge: both branches rewrite the SAME line.  No fences are
// rendered (DIS-080) — the merged bytes are just the produce at the merge
// commit, with the concurrent siblings laid out in RGA order.
{
  const w = abc.ram("CFOLD", 1 << 16);
  w.fold(null, E("a\nb\nc\n"), "c", C1, []);
  const wo = abc.ram("CFOLD", 1 << 16);
  wo.fold(w, E("a\nO\nc\n"), "c", C2, [C1]);
  const wt = abc.ram("CFOLD", 1 << 16);
  wt.fold(wo, E("a\nT\nc\n"), "c", C3, [C1]);
  const wm = abc.ram("CFOLD", 1 << 16);
  wm.merge(wt, C4, [C1, C2, C3]);
  const got = prod(wm, C4);
  if (got.indexOf("<<<<") >= 0) fail("conflict fences are retired (DIS-080): " + JSON.stringify(got));
  // the shared "\n" is context, so only the two word tokens are concurrent and
  // they land side by side: theirs (higher id) first, then ours.
  eq(got, "a\nTO\nc\n", "concurrent edits interleave in RGA order, unfenced");
}

io.log("weave.js OK");
