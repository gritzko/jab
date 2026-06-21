"use strict";
// WEAVE family: one file's whole DAG history as a 'W' TLV blob in a u8 buffer.
// fold = WEAVENext (diff a revision in), merge = WEAVEMerge, alive/produce emit
// bytes at the tip / any rev-scope, and the rewind()/next() cursor is WEAVEStep.
// Mirrors dog/test/WEAVE01.c: from-blob round-trip, diff fold, fork/merge.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + JSON.stringify(a) + " !== " + JSON.stringify(b)); }
const dec = (u8) => utf8.Decode(u8);
const E = (s) => utf8.Encode(s);
const C1 = "0000000000000001", C2 = "0000000000000002", C3 = "0000000000000003", C4 = "0000000000000004";

function alive(w) { const o = io.buf(1 << 16); w.alive(o); return dec(o.data()); }
function prod(w, ids) { const o = io.buf(1 << 16); w.produce(w.scope(ids), o); return dec(o.data()); }

// --- from-blob round-trip: WEAVEAlive of a from-blob weave reproduces the blob.
{
  const cases = ["", "abc", "int x = 1;\n", "int x = 1;", "a\nb\nc\n",
                 "int main(void){\n    return 0;\n}\n", "\n\n\nx\n"];
  for (const src of cases) {
    const w = abc.ram("WEAVE", 1 << 18);
    w.fold(null, E(src), "c", "00000000c0ffee01");
    eq(alive(w), src, "roundtrip " + JSON.stringify(src));
    eq(w.empty(), src.length === 0, "empty " + JSON.stringify(src));
  }
}

// --- a from-blob weave is one spine commit; all tokens are spine inserts.
{
  const w = abc.ram("WEAVE", 1 << 16);
  w.fold(null, E("a\nb\nc\n"), "c", "00000000c0ffee01");
  const cs = w.commits;
  eq(cs.length, 1, "from-blob has 1 commit");
  eq(cs[0].length, 16, "commit is a 16-char hashlet");
  // cursor: walk tokens, concatenated bytes == alive, all inserter 0 (spine).
  let txt = "", n = 0;
  w.rewind();
  while (w.next()) {
    n++;
    txt += dec(w.tokText);
    eq(w.inserter, 0, "from-blob token inserter is spine");
    eq(w.rms.length, 0, "from-blob token alive (no removers)");
    eq(w.anchor.length, 16, "anchor is a 16-char hashlet");
  }
  eq(n, w.size, "cursor visited every token");
  eq(txt, "a\nb\nc\n", "token bytes concatenate to the file");
  w.rewind();
  if (!w.next()) fail("rewind then next");
}

// --- diff fold: fold v2 onto v1.  tip == v2; rev{1} == v1; rev{1,2} == v2.
{
  const w1 = abc.ram("WEAVE", 1 << 16); w1.fold(null, E("a\nb\nc\n"), "c", C1);
  const w2 = abc.ram("WEAVE", 1 << 16); w2.fold(w1, E("a\nB\nc\n"), "c", C2);
  eq(alive(w2), "a\nB\nc\n", "diff tip");
  eq(prod(w2, [C1]), "a\nb\nc\n", "diff rev{1}");
  eq(prod(w2, [C1, C2]), "a\nB\nc\n", "diff rev{1,2}");
  eq(w2.commits.length, 2, "folded weave has 2 commits");
}

// --- fork/merge: base v0; branch A edits line 1, branch B edits line 3.
{
  const w0 = abc.ram("WEAVE", 1 << 16); w0.fold(null, E("a\nb\nc\n"), "c", C1);
  const wA = abc.ram("WEAVE", 1 << 16); wA.fold(w0, E("A\nb\nc\n"), "c", C2);
  const wB = abc.ram("WEAVE", 1 << 16); wB.fold(w0, E("a\nb\nC\n"), "c", C3);
  const wM = abc.ram("WEAVE", 1 << 16); wM.merge(wA, wB, C4);
  eq(alive(wM), "A\nb\nC\n", "merge tip combines both branches");
  eq(prod(wM, [C1, C2]), "A\nb\nc\n", "merge rev branch A");
  eq(prod(wM, [C1, C3]), "a\nb\nC\n", "merge rev branch B");
}

// --- emit a diff as HUNK records: line 4 d->X, from {c1} to {c1,c2}.  The
// emitted hunk reads/renders through the existing HUNK cursor (mirrors
// dog/test/WEAVE02.c emit_singlewin: one window, Diff body == Full body).
{
  const w1 = abc.ram("WEAVE", 1 << 16); w1.fold(null, E("a\nb\nc\nd\ne\nf\ng\n"), "c", C1);
  const w2 = abc.ram("WEAVE", 1 << 16); w2.fold(w1, E("a\nb\nc\nX\ne\nf\ng\n"), "c", C2);
  const from = w2.scope([C1]), to = w2.scope([C1, C2]);

  const hd = abc.ram("HUNK", 1 << 16);
  w2.emitDiff(from, to, "foo.c", "deadbeef", hd);
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
  w2.emitFull(from, to, "foo.c", "diff:", "deadbeef", hf);
  let m = 0;
  hf.rewind();
  while (hf.next()) { m++; eq(dec(hf.text), "a\nb\nc\nXd\ne\nf\ng\n", "full-diff body"); }
  eq(m, 1, "single-window full-diff is one hunk");
}

// --- merged: N-way conflict render (mirrors dog/test/WEAVE02.c emit_merged).
// Concurrent siblings lay out in RGA order (commit-id DESC), so theirs (c3)
// precedes ours (c2) inside the frame.  Disjoint edits merge with no markers.
{
  const w0 = abc.ram("WEAVE", 1 << 16); w0.fold(null, E("a\nb\nc\n"), "c", C1);
  const wo = abc.ram("WEAVE", 1 << 16); wo.fold(w0, E("a\nO\nc\n"), "c", C2);
  const wt = abc.ram("WEAVE", 1 << 16); wt.fold(w0, E("a\nT\nc\n"), "c", C3);
  const wm = abc.ram("WEAVE", 1 << 16); wm.merge(wo, wt, "0000000000000000");
  const o = io.buf(1 << 14);
  wm.merged([wm.scope([C1, C2]), wm.scope([C1, C3])], o);
  eq(dec(o.data()), "a\n<<<<T||||O>>>>\nc\n", "conflicting edits framed");

  const d0 = abc.ram("WEAVE", 1 << 16); d0.fold(null, E("a\nb\nc\n"), "c", C1);
  const da = abc.ram("WEAVE", 1 << 16); da.fold(d0, E("A\nb\nc\n"), "c", C2);
  const db = abc.ram("WEAVE", 1 << 16); db.fold(d0, E("a\nb\nC\n"), "c", C3);
  const dm = abc.ram("WEAVE", 1 << 16); dm.merge(da, db, "0000000000000000");
  const d = io.buf(1 << 14);
  dm.merged([dm.scope([C1, C2]), dm.scope([C1, C3])], d);
  eq(dec(d.data()), "A\nb\nC\n", "disjoint edits merge with no markers");
}

// --- weaveIdHash: deterministic 16-char hashlet, varies with (commit, ordinal).
{
  const a = abc.weaveIdHash(C1, 0), b = abc.weaveIdHash(C1, 1), c = abc.weaveIdHash(C2, 0);
  eq(a.length, 16, "idhash is a 16-char hashlet");
  eq(a, abc.weaveIdHash(C1, 0), "idhash deterministic");
  if (a === b) fail("idhash should vary with ordinal");
  if (a === c) fail("idhash should vary with commit");
}

io.log("weave.js OK");
