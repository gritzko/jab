"use strict";
// tok.parse(bytes, lang) -> tok32 Uint32Array + the pure-JS TokStream cursor
// (JS-023).  Mirrors the dog/tok lexer that HUNK.dogenize drives, but exposes
// the bare tok32 array + a cursor instead of serializing a hunk.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }
const dec = (u8) => utf8.Decode(u8);

// parse a "js" snippet: known tag/start/end/text on a couple of tokens
{
  const src = utf8.Encode("let x = 42;\n");
  const t32 = tok.parse(src, "js");
  if (!(t32 instanceof Uint32Array)) fail("parse returns Uint32Array");
  eq(t32.length, 9, "js token count");

  const s = new TokStream(t32, src);
  eq(s.length, 9, "stream length");

  // token 0: keyword "let" [0,3)
  eq(s.tag, "R", "tok0 tag");
  eq(s.start, 0, "tok0 start");
  eq(s.end, 3, "tok0 end");
  eq(s.side, 0, "tok0 side");
  eq(s.custom, 0, "tok0 custom");
  eq(dec(s.text(src)), "let", "tok0 text");
  eq(s.str(src), "let", "tok0 str");

  // walk to token 6: number "42" [8,10)
  let i = 0;
  while (i < 6) { if (!s.next()) fail("ran out at " + i); i++; }
  eq(s.tag, "L", "tok6 tag");
  eq(s.start, 8, "tok6 start");
  eq(s.end, 10, "tok6 end");
  eq(s.str(src), "42", "tok6 str");

  // seek to token 7: punct ";" [10,11)
  s.seek(7);
  eq(s.tag, "P", "tok7 tag");
  eq(s.start, 10, "tok7 start");
  eq(s.end, 11, "tok7 end");
  eq(s.str(src), ";", "tok7 str");

  // full walk reaches every token and stops cleanly
  s.seek(0);
  let n = 1;
  while (s.next()) n++;
  eq(n, 9, "full walk count");
}

// text() is a zero-copy subarray over the pinned source
{
  const src = utf8.Encode("a=b");
  const s = new TokStream(tok.parse(src, "js"), src);
  const v = s.text(src);
  if (!(v instanceof Uint8Array)) fail("text() is a Uint8Array");
  if (v.buffer !== src.buffer) fail("text() must be zero-copy over src");
}

// unknown / empty lang -> plain-text fallback (still tokenizes, no throw)
{
  const src = utf8.Encode("hello world");
  const a = tok.parse(src, "no-such-lang");
  if (a.length === 0) fail("unknown lang should fall back to plain text");
  const b = tok.parse(src, "");
  if (b.length === 0) fail("empty lang should fall back to plain text");
}

// empty source -> empty array
{
  const e = tok.parse(utf8.Encode(""), "js");
  if (!(e instanceof Uint32Array)) fail("empty -> Uint32Array");
  eq(e.length, 0, "empty source -> empty array");
}

// source > 16 MiB (24-bit offset cap) -> throws
{
  let threw = false;
  const big = new Uint8Array((1 << 24) + 1);   // 16 MiB + 1
  try { tok.parse(big, "js"); } catch (e) { threw = true; }
  if (!threw) fail(">16 MiB source must throw");
}

// (JS-023 out param) tokenizing into an `out` Buf matches the no-out result:
// same token count + per-token tag/start/end/text.
{
  const src = utf8.Encode("let x = 42;\n");
  const ref = tok.parse(src, "js");                 // no-out (fresh array)
  const out = io.buf(4096);
  const got = tok.parse(src, "js", out);            // into out
  if (!(got instanceof Uint32Array)) fail("out parse returns Uint32Array");
  eq(got.length, ref.length, "out token count == no-out");
  const a = new TokStream(ref, src), b = new TokStream(got, src);
  for (let i = 0; i < ref.length; i++) {
    a.seek(i); b.seek(i);
    eq(b.tag, a.tag, "out tok" + i + " tag");
    eq(b.start, a.start, "out tok" + i + " start");
    eq(b.end, a.end, "out tok" + i + " end");
    eq(dec(b.text(src)), dec(a.text(src)), "out tok" + i + " text");
  }
}

// the returned view is zero-copy over the `out` Buf's bytes
{
  const src = utf8.Encode("a=b");
  const out = io.buf(256);
  const got = tok.parse(src, "js", out);
  if (got.buffer !== out.bytes.buffer) fail("out view must share out's buffer");
  // out's cursor advanced by exactly the bytes written (4 per token)
  eq(out.size, got.length * 4, "out cursor advanced by n*4");
}

// reuse one `out` Buf across two parses (reset between); 2nd result is correct
{
  const out = io.buf(4096);
  const s1 = utf8.Encode("let a = 1;\n");
  const g1 = tok.parse(s1, "js", out);
  const c1 = g1.length;
  out.reset();                                       // back to an aligned base
  const s2 = utf8.Encode("const bb = 22;\n");
  const g2 = tok.parse(s2, "js", out);
  // g2 must match a fresh no-out parse of s2
  const ref2 = tok.parse(s2, "js");
  eq(g2.length, ref2.length, "reused out: 2nd token count");
  const a = new TokStream(ref2, s2), b = new TokStream(g2, s2);
  for (let i = 0; i < ref2.length; i++) {
    a.seek(i); b.seek(i);
    eq(b.tag, a.tag, "reuse tok" + i + " tag");
    eq(b.end, a.end, "reuse tok" + i + " end");
    eq(dec(b.text(s2)), dec(a.text(s2)), "reuse tok" + i + " text");
  }
  if (c1 < 1) fail("reused out: 1st parse produced no tokens");
}

// a too-small `out` throws (before any partial write corrupts it)
{
  let threw = false;
  const src = utf8.Encode("let x = 42;\n");
  const tiny = io.buf(4);                             // room for one tok32 only
  try { tok.parse(src, "js", tiny); } catch (e) { threw = true; }
  if (!threw) fail("too-small out must throw");
}

// an unaligned `out` (IDLE head not 4-aligned) throws a clear error
{
  let threw = false;
  const out = io.buf(256);
  out.feed1(0x7a);                                    // advance IDLE by 1 byte -> unaligned
  const src = utf8.Encode("a=b");
  try { tok.parse(src, "js", out); } catch (e) { threw = true; }
  if (!threw) fail("unaligned out must throw");
}

io.log("tok.js OK");
