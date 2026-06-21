#ifndef JABC_TOK_HPP
#define JABC_TOK_HPP
//  tok binding — the JS face of dog/tok's TOKLexer.  ONE native leaf,
//  _tok_parse_into(srcBytes, lang, outU8) -> tokenCount, sharing the SAME C
//  core (HUNKu32bTokenize) as HUNK.dogenize — no parse logic is duplicated.
//  The leaf writes the packed tok32 STRAIGHT into the caller's region (a Buf's
//  IDLE or a fresh worst-case scratch); the JS dispatch (tok.parse) returns a
//  zero-copy Uint32Array view over the bytes just written.  The pure-JS
//  TokStream cursor (embedded JS below) decodes tag/start/end/side/custom by
//  bit math over that array.  See JS-023.
#include "hunk.hpp"  //  JABCArgU8 (the lang ext arg) + dog/HUNK.h core

//  _tok_parse_into(srcBytes, lang, outU8) -> tokenCount
//  Mirrors JABChunkDogenize's lex half but writes packed tok32 directly into
//  the caller-owned region `outU8` (a Buf's idle() or a fresh JS scratch),
//  returning the token count.  Guards, ALL before any write:
//   - source > 16 MiB -> throw (24-bit end-offset cap, HUNKTOKOOB);
//   - outU8 not 4-byte aligned -> throw (a u32 view over it must be aligned;
//     a fresh/reset Buf has a 4-aligned IDLE head — callers reuse via reset());
//   - outU8 too small for the worst case ((srcn+1) tok32) -> throw, so a partial
//     lex can never corrupt a reused buffer.
//  The lexer runs in place on `outU8`; the caller advances its cursor by n*4.
static JABC_FN(JABCtokParseInto) {
  if (argc < 3) JABC_THROW("tok._tok_parse_into(srcBytes, lang, outU8)");
  u8s source = {};
  if (!JABCBytesOf(source, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t srcn = $len(source);
  if (srcn > TOK_OFF_MASK)  //  24-bit end offset cap (16 MiB) -> HUNKTOKOOB
    JABC_THROW("tok.parse: source > 16 MiB");
  u8 exttmp[64];
  u8s ext = {};
  if (!JABCArgU8(ext, ctx, args[1], exttmp, sizeof(exttmp), exception))
    return JSValueMakeUndefined(ctx);
  u8s out = {};
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  //  A zero-copy Uint32Array view over the written bytes needs a 4-aligned
  //  write position; validate before running so a misuse is a clean throw,
  //  not a corrupt/aliased array.
  if (((uintptr_t)out[0] & 3u) != 0)
    JABC_THROW("tok.parse: out not 4-byte aligned (reset the Buf)");
  //  (srcn+1) tok32 is the per-byte upper bound on token count (each token
  //  spans >= 1 byte; +1 covers the empty edge).  Require room up front.
  size_t need = (srcn + 1) * sizeof(u32);
  if ($len(out) < need) JABC_THROW("tok.parse: out too small");
  u32* base = (u32*)out[0];
  u32* tb[4] = {base, base, base, base + (srcn + 1)};
  u8cs srcc = {source[0], source[1]};
  u8cs extc = {ext[0], ext[1]};
  ok64 o = HUNKu32bTokenize(tb, srcc, extc);  //  shared core with HUNK.dogenize
  if (o != OK) JABC_THROW("tok.parse: lex");
  size_t n = (size_t)(tb[2] - tb[1]);  //  tb[1]==base, so this is the count
  return JSValueMakeNumber(ctx, (double)n);
}

//  tok.parse(srcBytes, lang, out?) + the pure-JS TokStream cursor.  Decode is
//  bit math over the Uint32Array mirroring tok32Tag/Offset/Side (dog/tok/TOK.h);
//  token i's start = token i-1's end (0 for i==0).  The cursor PINS the source
//  Uint8Array (offsets are positions, not bytes); text() is a zero-copy
//  subarray, str() decodes it via utf8.Decode.
//
//  out (optional, a Buf): tokenize packed tok32 straight into out's IDLE,
//  advance out.fed(n*4), and return a ZERO-COPY Uint32Array view over those
//  bytes — so one Buf can be reused across many parses (reset() between).  The
//  view is valid only while out is not further fed/reset.  Without out, a fresh
//  worst-case scratch is allocated and the result returned as its own (fresh,
//  4-aligned) trimmed Uint32Array — unchanged from before.
static const char* JABC_TOK_JS = R"JS(
(function (g) {
  "use strict";
  const tok = g.tok, utf8 = g.utf8;
  tok.parse = (bytes, lang, out) => {
    lang = lang || "";
    if (out !== undefined && out !== null) {
      //  reuse the caller's Buf: lex into its IDLE, commit, view it zero-copy.
      const idle = out.idle();                     // [ _idle, cap ) as a Uint8Array
      const n = tok._tok_parse_into(bytes, lang, idle);
      out.fed(n * 4);                              // advance the cursor n tok32
      //  zero-copy u32 view over the bytes just written (idle head is 4-aligned;
      //  the native leaf threw otherwise).
      return new Uint32Array(idle.buffer, idle.byteOffset, n);
    }
    //  no out: a fresh worst-case scratch ((srcLen+1) tok32), trimmed to n.
    const scratch = new Uint8Array((bytes.length + 1) * 4);
    const n = tok._tok_parse_into(bytes, lang, scratch);
    return new Uint32Array(scratch.buffer, 0, n);  // fresh, nothing else aliases
  };
  class TokStream {
    constructor(t32, src) {
      this._t = t32;            // Uint32Array of tok32
      this._src = src;          // pinned source Uint8Array (positions index it)
      this._i = 0;
    }
    get length() { return this._t.length; }
    get _w()  { return this._t[this._i] >>> 0; }
    get tag()    { return String.fromCharCode(65 + (this._w >>> 27)); }
    get custom() { return (this._w >>> 26) & 1; }
    get side()   { return (this._w >>> 24) & 3; }
    get end()    { return this._w & 0xFFFFFF; }
    get start()  { return this._i > 0 ? (this._t[this._i - 1] & 0xFFFFFF) : 0; }
    text(src) {                 // zero-copy subarray over the source bytes
      const s = src || this._src;
      return s.subarray(this.start, this.end);
    }
    str(src) { return utf8.Decode(this.text(src)); }
    seek(i) { this._i = i | 0; return this; }
    next() {
      if (this._i + 1 >= this._t.length) return false;
      this._i++;
      return true;
    }
  }
  tok.TokStream = TokStream;
  g.TokStream = TokStream;
})(this);
)JS";

#endif
