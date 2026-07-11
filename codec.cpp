#include "JABC.hpp"
#include "cont.hpp"  //  JABCBlob (engine Uint8Array copy)
extern "C" {
#include "abc/HEX.h"
#include "abc/RON.h"
#include "abc/SHA.h"
#include "dog/DOG.h"      //  DOGutf8sFeedDate (relative-date formatter, JS-021)
#include "dog/git/SHA1.h"
}
#include <time.h>

//  hex + sha: pure byte transforms (no state).  hex mirrors utf8 (a string on
//  the text side, bytes on the binary side); sha1/sha256 hash bytes -> the
//  sha1 lane (Uint8Array(20)/(32)).

//  hex.encode(Uint8Array) -> lowercase hex string
static JABC_FN(JABChexEncode) {
  if (argc < 1) JABC_THROW("hex.encode(Uint8Array)");
  u8s bin = {};
  if (!JABCBytesOf(bin, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t n = (size_t)$len(bin);
  char* h = (char*)malloc(n * 2 + 1);
  if (h == NULL) JABC_THROW("hex.encode: oom");
  u8s hx = {(u8*)h, (u8*)h + n * 2};
  u8cs b = {bin[0], bin[1]};
  HEXu8sFeedSome(hx, b);
  h[n * 2] = 0;
  JSStringRef js = JSStringCreateWithUTF8CString(h);
  free(h);
  JSValueRef v = JSValueMakeString(ctx, js);
  JSStringRelease(js);
  return v;
}

//  hex.encodeInto(src, dst) -> hex chars written (provided-buffer form)
static JABC_FN(JABChexEncodeInto) {
  if (argc < 2) JABC_THROW("hex.encodeInto(src, dst)");
  u8s bin = {}, dst = {};
  if (!JABCBytesOf(bin, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(dst, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  u8s hx = {dst[0], dst[1]};
  u8cs b = {bin[0], bin[1]};
  HEXu8sFeedSome(hx, b);
  return JSValueMakeNumber(ctx, (double)(size_t)(hx[0] - dst[0]));
}

//  hex.decode(string) -> Uint8Array
static JABC_FN(JABChexDecode) {
  if (argc < 1 || !JSValueIsString(ctx, args[0])) JABC_THROW("hex.decode(string)");
  JSStringRef s = JSValueToStringCopy(ctx, args[0], exception);
  if (*exception || s == NULL) return JSValueMakeUndefined(ctx);
  size_t max = JSStringGetMaximumUTF8CStringSize(s);
  char* hb = (char*)malloc(max);
  if (hb == NULL) { JSStringRelease(s); JABC_THROW("hex.decode: oom"); }
  size_t got = JSStringGetUTF8CString(s, hb, max);
  JSStringRelease(s);
  size_t hlen = got ? got - 1 : 0;  //  drop the NUL the API counts
  u8cs hex = {(u8*)hb, (u8*)hb + hlen};
  if ((hlen & 1) || !HEXu8sValid(hex)) { free(hb); JABC_THROW("hex.decode: bad hex"); }
  size_t n = hlen / 2;
  JSObjectRef ta =
      JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, n, exception);
  if (*exception || ta == NULL) { free(hb); return JSValueMakeUndefined(ctx); }
  u8* p = (u8*)JSObjectGetTypedArrayBytesPtr(ctx, ta, exception);
  if (p && n) {
    u8s bin = {p, p + n};
    HEXu8sDrainSome(bin, hex);
  }
  free(hb);
  return ta;
}

//  sha1(Uint8Array) -> Uint8Array(20)
static JABC_FN(JABCsha1) {
  if (argc < 1) JABC_THROW("sha1(Uint8Array)");
  u8s bin = {};
  if (!JABCBytesOf(bin, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  sha1 h = {};
  u8cs from = {bin[0], bin[1]};
  SHA1Sum(&h, from);
  return JABCBlob(ctx, h.data, sizeof(h.data));
}

//  sha256(Uint8Array) -> Uint8Array(32)
static JABC_FN(JABCsha256) {
  if (argc < 1) JABC_THROW("sha256(Uint8Array)");
  u8s bin = {};
  if (!JABCBytesOf(bin, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  sha256 h = {};
  u8cs from = {bin[0], bin[1]};
  SHASum(&h, from);
  return JABCBlob(ctx, h.data, sizeof(h.data));
}

//  ron.encode(BigInt) -> RON base64 string  (timestamps, verbs, ok64 codes)
static JABC_FN(JABCronEncode) {
  if (argc < 1) JABC_THROW("ron.encode(BigInt)");
  uint64_t v = JSValueToUInt64(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  u8 b[16];
  u8s into = {b, b + sizeof(b)};
  RONutf8sFeed(into, (ok64)v);
  //  JS-108: shared conversion (RON60 text is <= 10 bytes by construction).
  u8cs s = {b, into[0]};
  return JABCStrOfSlice(ctx, s, exception);
}

//  ron.decode(string) -> BigInt
static JABC_FN(JABCronDecode) {
  if (argc < 1 || !JSValueIsString(ctx, args[0])) JABC_THROW("ron.decode(string)");
  JSStringRef s = JSValueToStringCopy(ctx, args[0], exception);
  if (*exception || s == NULL) return JSValueMakeUndefined(ctx);
  u8 b[32];
  size_t got = JSStringGetUTF8CString(s, (char*)b, sizeof(b));
  JSStringRelease(s);
  size_t n = got ? got - 1 : 0;
  u8cs from = {b, b + n};
  ok64 v = 0;
  RONutf8sDrain(&v, from);
  return JSBigIntCreateWithUInt64(ctx, (uint64_t)v, exception);
}

//  ron time interpretation (JS-021): ron60 IS the ULOG `ts` encoding.  Three
//  thin leaves; the Date-coerce + BigInt sugar lives in JS (JABC_RON_JS).
//  ron60 crosses as BigInt, like ron.encode/decode.

//  ron._now() -> current ron60 (BigInt), localtime-aligned ms (RONNow).
static JABC_FN(JABCronNow) {
  (void)argc; (void)args;
  return JSBigIntCreateWithUInt64(ctx, (uint64_t)RONNow(), exception);
}

//  ron._ofMs(ms) -> ron60 (BigInt) for a ms-epoch int.  localtime split +
//  RONOfTime, matching RONNow's wall-clock alignment.
static JABC_FN(JABCronOfMs) {
  if (argc < 1) JABC_THROW("ron._ofMs(ms)");
  double msd = JSValueToNumber(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  i64 msi = (i64)msd;
  time_t sec = (time_t)(msi / 1000);
  u32 ms = (u32)(((msi % 1000) + 1000) % 1000);  //  floor-mod for pre-epoch
  struct tm tm = {};
  localtime_r(&sec, &tm);
  ron60 r = 0;
  if (RONOfTime(&r, &tm, ms) != OK) JABC_THROW("ron._ofMs: out of range");
  return JSBigIntCreateWithUInt64(ctx, (uint64_t)r, exception);
}

//  ron._date(ron60) -> relative-date string.  RONToTime -> mktime -> unix
//  secs, now=time(NULL), DOGutf8sFeedDate (the be-log "12:34"/"Tue05" format).
static JABC_FN(JABCronDate) {
  if (argc < 1) JABC_THROW("ron._date(BigInt)");
  uint64_t v = JSValueToUInt64(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  i64 secs = 0;
  if (v != 0) {
    struct tm tm = {};
    if (RONToTime((ron60)v, &tm, NULL) != OK) JABC_THROW("ron._date: bad ron60");
    tm.tm_isdst = -1;                 //  let mktime resolve DST (cf. SNIFF)
    time_t s = mktime(&tm);
    secs = (s == (time_t)-1) ? 0 : (i64)s;
  }
  u8 b[16];
  u8s into = {b, b + sizeof(b)};
  if (DOGutf8sFeedDate(into, secs, (i64)time(NULL)) != OK)
    JABC_THROW("ron._date: format failed");
  //  JS-108: shared conversion (a DOG date is <= 16 bytes by construction).
  u8cs s = {b, into[0]};
  return JABCStrOfSlice(ctx, s, exception);
}

//  JS sugar: now() binds the leaf; of() coerces Date->getTime(); date()
//  coerces to BigInt.  No held JS refs (JABC rule #4).
static const char* JABC_RON_JS = R"JS(
(function (g) {
  "use strict";
  const ron = g.ron;
  ron.now = () => ron._now();
  ron.of = x => ron._ofMs(x instanceof Date ? x.getTime() : Number(x));
  ron.date = r => ron._date(BigInt(r));
})(this);
)JS";

ok64 JABCCodecInstall() {
  JABC_API_OBJECT(ron);
  JABC_API_FN(ron, "encode", JABCronEncode);
  JABC_API_FN(ron, "decode", JABCronDecode);
  JABC_API_FN(ron, "_now", JABCronNow);    //  ron.now/of/date sugar: JABC_RON_JS
  JABC_API_FN(ron, "_ofMs", JABCronOfMs);
  JABC_API_FN(ron, "_date", JABCronDate);
  JABC_API_OBJECT(hex);
  JABC_API_FN(hex, "encode", JABChexEncode);
  JABC_API_FN(hex, "encodeInto", JABChexEncodeInto);
  JABC_API_FN(hex, "decode", JABChexDecode);
  JABC_API_FN(JABC_GLOBAL_OBJECT, "sha1", JABCsha1);
  JABC_API_FN(JABC_GLOBAL_OBJECT, "sha256", JABCsha256);
  JABCExecute(JABC_RON_JS);
  return OK;
}
