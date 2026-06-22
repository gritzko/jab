#include "JABC.hpp"
extern "C" {
#include "dog/git/ZINF.h"   //  ZINFDeflate / ZINFInflate (libdog, already linked)
}

//  zip: raw zlib (de)compression of arbitrary bytes (JS-035) — the missing
//  primitive for a pure-JS git-wire client (loose objects, REF_DELTA bodies).
//  Two RAW leaves marshal typed arrays into u8s and call one dog/git/ZINF
//  function each; the out buffer is JS-owned (the leaf sizes nothing).  The
//  size-and-grow sugar (zip.deflate/inflate) is the embedded JABC_ZIP_JS.

//  NOTE on inflate capacity: ZINFInflate does NOT report NOROOM — when the
//  out region fills it WRAPS next_out and keeps going, then the trailing
//  u8sFed(total_out) silently fails so the head never advances (produced 0).
//  A genuine empty result also yields produced 0.  We disambiguate with a
//  head sentinel: on overflow ZINF wrote the wrapped tail over out[off], so
//  the sentinel is gone; on a real empty result nothing was written.

//  zip._deflate(src, out, outOff) -> bytes produced into out at outOff.
static JABC_FN(JABCzipDeflate) {
  if (argc < 3) JABC_THROW("zip._deflate(src, out, outOff)");
  u8s src = {}, out = {};
  if (!JABCBytesOf(src, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(out, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[2], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (off > (size_t)$len(out)) JABC_THROW("zip.deflate: outOff past end");
  u8s into = {out[0] + off, out[1]};
  u8cs s = {src[0], src[1]};
  if (ZINFDeflate(into, s) != OK) JABC_THROW("zip.deflate: failed (out too small?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - (out[0] + off)));
}

//  zip._inflate(src, out, outOff) -> bytes produced into out at outOff.
//  Throws NOROOM-style on a too-small out so the JS sugar grows and retries.
static JABC_FN(JABCzipInflate) {
  if (argc < 3) JABC_THROW("zip._inflate(src, out, outOff)");
  u8s src = {}, out = {};
  if (!JABCBytesOf(src, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(out, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[2], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (off > (size_t)$len(out)) JABC_THROW("zip.inflate: outOff past end");
  u8s into = {out[0] + off, out[1]};
  size_t room = (size_t)$len(into);
  u8 sentinel = 0;
  if (room) { sentinel = (u8)(into[0][0] ^ 0xa5); into[0][0] = sentinel; }
  u8cs s = {src[0], src[1]};
  if (ZINFInflate(into, s) != OK) JABC_THROW("zip.inflate: bad zlib stream");
  size_t produced = (size_t)(into[0] - (out[0] + off));
  //  produced 0 + a non-empty out region whose head was overwritten == ZINF
  //  wrapped (out too small): throw so the sugar grows.  Head intact == a
  //  genuine empty result.
  if (produced == 0 && room && (out[0] + off)[0] != sentinel)
    JABC_THROW("zip.inflate: NOROOM (out too small)");
  return JSValueMakeNumber(ctx, (double)produced);
}

//  JS sugar: zip.deflate/inflate(bytes, out?).  No out -> a fresh sized
//  Uint8Array (deflate: len+len>>10+128 scratch, sliced to n; inflate:
//  max(64,len*4), grow-and-retry x N on the leaf throwing NOROOM, like
//  keeper.js readRecord).  An out Buf -> write into idle(), advance fed(n),
//  return n (zero-copy).  No held refs (JABC rule #4).
static const char* JABC_ZIP_JS = R"JS(
(function (g) {
  "use strict";
  const zip = g.zip;
  const isBuf = (o) => o && typeof o === "object" &&
    typeof o.idle === "function" && typeof o.fed === "function";

  zip.deflate = (bytes, out) => {
    if (isBuf(out)) {
      const n = zip._deflate(bytes, out.idle(), 0);
      out.fed(n);
      return n;
    }
    const cap = bytes.length + (bytes.length >> 10) + 128;
    const scratch = new Uint8Array(cap);
    const n = zip._deflate(bytes, scratch, 0);
    return scratch.slice(0, n);
  };

  zip.inflate = (bytes, out) => {
    if (isBuf(out)) {
      let cap = Math.max(64, bytes.length * 4), tries = 8;
      while (true) {
        if (out.room < cap) out.grow(out.cap + (cap - out.room));
        try {
          const n = zip._inflate(bytes, out.idle(), 0);
          out.fed(n);
          return n;
        } catch (e) {
          if (--tries <= 0 || cap > (1 << 30)) throw e;
          cap *= 2;
        }
      }
    }
    let cap = Math.max(64, bytes.length * 4), tries = 8;
    while (true) {
      const scratch = new Uint8Array(cap);
      try {
        const n = zip._inflate(bytes, scratch, 0);
        return scratch.slice(0, n);
      } catch (e) {
        if (--tries <= 0 || cap > (1 << 30)) throw e;
        cap *= 2;
      }
    }
  };
})(this);
)JS";

ok64 JABCZipInstall() {
  JABC_API_OBJECT(zip);
  JABC_API_FN(zip, "_deflate", JABCzipDeflate);  //  zip.deflate sugar: JABC_ZIP_JS
  JABC_API_FN(zip, "_inflate", JABCzipInflate);  //  zip.inflate sugar: JABC_ZIP_JS
  JABCExecute(JABC_ZIP_JS);
  return OK;
}
