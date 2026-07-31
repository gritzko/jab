#ifndef JABC_CFOLD_HPP
#define JABC_CFOLD_HPP
//  CFOLD (DIS-082) — one file's whole DAG history as a 'V' TLV blob (the
//  APPEND-ONLY weave).  A CFOLD container is a JS-owned u8 buffer holding ONE
//  'V' blob; the binding parses it zero-copy per call (stateless leaves, like
//  HUNK).  The builders (_cfold_next/_cfold_merge) write a FRESH 'V' blob into
//  a target buffer.
//
//  Hashes are STRINGS (JABC convention): a commit id is the hi64 of the commit
//  sha1, presented as a 16-char hex hashlet.  Every u64<->hex conversion lives
//  here; no u64 ever crosses the boundary as a JS number/BigInt.  Small values
//  that fit a double (commit INDEX, body offset, counts) stay plain numbers.
//
//  Scope bitmaps and the token identity hash are GONE: visibility is stored
//  per commit and identity IS the body offset.  The step cursor stays —
//  _cfold_step iterates the DFS with its state in a JS-owned Uint32Array.
//
//  hunk.hpp must precede this header (shared JABCArgU8 / JABCSubU8).
#include "cont.hpp"

extern "C" {
#include "dog/CFOLD.h"
}

//  --- u64 <-> 16-char hex hashlet (big-endian: first sha byte = top bits) ---
//  A commit id is be64(sha1[0..8]); its 16 hex chars are the value's hex.
static inline u64 JABCcfoldHi64(JSContextRef ctx, JSValueRef v, JSValueRef* ex) {
  JSStringRef s = JSValueToStringCopy(ctx, v, ex);
  if (*ex || s == NULL) return 0;
  char b[64];
  size_t n = JSStringGetUTF8CString(s, b, sizeof(b));
  JSStringRelease(s);
  if (n) n--;  // drop the NUL JSStringGetUTF8CString counts
  u64 h = 0;
  u32 got = 0;
  for (size_t i = 0; i < n && got < 16; i++) {
    char c = b[i];
    u32 d;
    if (c >= '0' && c <= '9') d = (u32)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
    else break;
    h = (h << 4) | d;
    got++;
  }
  return h;
}

//  u64 -> a fresh JS string of its 16-char lowercase hex hashlet.
static inline JSValueRef JABCcfoldHashlet(JSContextRef ctx, u64 h) {
  static const char HX[] = "0123456789abcdef";
  char b[17];
  for (int i = 15; i >= 0; i--) { b[i] = HX[h & 0xf]; h >>= 4; }
  b[16] = 0;
  JSStringRef s = JSStringCreateWithUTF8CString(b);
  JSValueRef v = JSValueMakeString(ctx, s);
  JSStringRelease(s);
  return v;
}

//  Parse the 'V' blob in (bv, lv=byte length) into `w`, also reporting the
//  blob base.  NO on a non-array or a malformed/empty blob.
static inline bool JABCcfoldAt(cfold* w, u8** base, JSContextRef ctx,
                               JSValueRef bv, JSValueRef lv, JSValueRef* ex) {
  u8* blob[4] = {};
  if (!JABCDataOf(blob, ctx, bv, ex)) return false;
  size_t len = 0;                        //  gated, never past the view
  if (!JABCOffOf(&len, u8bDataC(blob), ctx, lv, ex)) return false;
  *base = u8bData(blob)[0];
  u8cs bc = {};
  if (len > 0xffffffffUL || u8csSub(u8bDataC(blob), bc, 0, (u32)len) != OK)
    return false;
  return CFOLDParse(w, bc) == OK;
}

//  A commit HASHLET -> its build index in the 'C' table.  NO when the id was
//  never folded (the caller reports it in plain words).
static inline bool JABCcfoldRev(u32* out, cfold const* w, JSContextRef ctx,
                                JSValueRef v, JSValueRef* ex) {
  u64 id = JABCcfoldHi64(ctx, v, ex);
  return CFOLDFindCommit(out, w, id) == OK;
}

//  Read a JS array of hashlet strings into a malloc'd u64 vector; the caller
//  frees it.  *n is the element count (0 => NULL is fine).
static inline u64* JABCcfoldIds(u32* n, JSContextRef ctx, JSValueRef v,
                                JSValueRef* ex) {
  *n = 0;
  JSObjectRef a = JSValueToObject(ctx, v, ex);
  if (a == NULL) return NULL;
  JSStringRef ls = JSStringCreateWithUTF8CString("length");
  u32 an = (u32)JSValueToNumber(ctx, JSObjectGetProperty(ctx, a, ls, ex), ex);
  JSStringRelease(ls);
  if (an == 0) return NULL;
  u64* ids = (u64*)malloc((size_t)an * sizeof(u64));
  if (ids == NULL) return NULL;
  for (u32 i = 0; i < an; i++)
    ids[i] = JABCcfoldHi64(ctx, JSObjectGetPropertyAtIndex(ctx, a, i, ex), ex);
  *n = an;
  return ids;
}

//  Emit sink: append each emitted hunk as a TLV 'H' record into a HUNK
//  container's buffer (`into` advances per record).  The JABC rule #4 — C holds
//  no JS closure — holds: the callback is this C leaf, never a JS function.
typedef struct { u8s into; ok64 err; } JABCemit;
static ok64 JABCcfoldEmitCb(hunkc* hk, void* vctx) {
  JABCemit* c = (JABCemit*)vctx;
  ok64 o = HUNKu8sFeed(c->into, hk);
  if (o != OK) c->err = o;
  return o;
}

//  _cfold_count(blob, len) -> INDEX entry count (inserts + tombs + chain
//  terminators), 0 for an empty/unbuilt weave.  This is the `'E'` stream's
//  length, not the visible-token count — step the cursor for that.
static JABC_FN(JABCcfoldCount) {
  if (argc < 2) JABC_THROW("cfold._count(blob, len)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  return JSValueMakeNumber(ctx, (double)(u32)$len(w.idx));
}

//  _cfold_commits(blob, len) -> Array of 16-char hashlet strings, in BUILD
//  order (index i is the commit index every other leaf takes).
static JABC_FN(JABCcfoldCommits) {
  if (argc < 2) JABC_THROW("cfold._commits(blob, len)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSObjectMakeArray(ctx, 0, NULL, exception);
  u32 n = CFOLDNCommits(&w);
  if (n == 0) return JSObjectMakeArray(ctx, 0, NULL, exception);
  JSValueRef* els = (JSValueRef*)malloc(n * sizeof(JSValueRef));
  if (els == NULL) JABC_THROW("cfold.commits: out of memory");
  for (u32 i = 0; i < n; i++) {
    cfcommit c = {};
    if (CFOLDCommitAt(&c, &w, i) != OK) { free(els); JABC_THROW("cfold.commits: malformed weave"); }
    els[i] = JABCcfoldHashlet(ctx, c.id);
  }
  JSObjectRef arr = JSObjectMakeArray(ctx, n, els, exception);
  free(els);
  return arr;
}

//  _cfold_next(dest, base|null, baseLen, newBlob, ext, hash, ancestors[])
//  -> blob byte length.  Folds `newBlob` (tokenized by `ext`) onto `base` under
//  commit `hash`, writing a fresh 'V' blob from offset 0 of `dest`.  `ancestors`
//  is the new commit's WHOLE CAUSAL CLOSURE (itself excluded) as hashlets;
//  everything already folded and not named there lands in its ignore-set.
//  base NULL/empty => a from-blob weave.
static JABC_FN(JABCcfoldNext) {
  if (argc < 7) JABC_THROW("cfold._next(dest, base, baseLen, newBlob, ext, hash, ancestors)");
  u8* destb[4] = {};
  if (!JABCIdleOf(destb, ctx, args[0], exception)) JABC_UNDEF;
  cfold bw = {};
  cfold* wp = NULL;
  u8* bbase = NULL;
  if (!JSValueIsNull(ctx, args[1]) && !JSValueIsUndefined(ctx, args[1])) {
    if (JABCcfoldAt(&bw, &bbase, ctx, args[1], args[2], exception)) wp = &bw;
  }
  u8* nbb[4] = {};
  if (!JABCDataOf(nbb, ctx, args[3], exception)) JABC_UNDEF;
  u8 exttmp[64];
  u8s ext = {};
  if (!JABCArgU8(ext, ctx, args[4], exttmp, sizeof(exttmp), exception))
    return JSValueMakeUndefined(ctx);
  u64 commit = JABCcfoldHi64(ctx, args[5], exception);
  u32 an = 0;
  u64* ids = JABCcfoldIds(&an, ctx, args[6], exception);
  u8* const dbase = u8bIdle(destb)[0];
  u8s into = {dbase, u8bIdle(destb)[1]};
  u8csc extc = {ext[0], ext[1]};
  u64csc anc = {ids, ids + an};
  ok64 o = CFOLDFold(into, wp, u8bDataC(nbb), extc, commit, anc);
  free(ids);
  if (o == CFOLDBIG) JABC_THROW("cfold.fold: the file is too big to weave (over 16 MB of history)");
  if (o != OK) JABC_THROW("cfold.fold: failed (buffer full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - dbase));
}

//  _cfold_merge(dest, base, baseLen, hash, ancestors[]) -> blob byte length
//  A merge carries NO content of its own: ONE weave in (both sides are already
//  folded into it), one 'C' record out taking the later L and the intersected
//  ignore-set.  No weave pair, no renumbering.
static JABC_FN(JABCcfoldMerge) {
  if (argc < 5) JABC_THROW("cfold._merge(dest, base, baseLen, hash, ancestors)");
  u8* destb[4] = {};
  if (!JABCIdleOf(destb, ctx, args[0], exception)) JABC_UNDEF;
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[1], args[2], exception))
    JABC_THROW("cfold.merge: the base weave is empty or malformed");
  u64 commit = JABCcfoldHi64(ctx, args[3], exception);
  u32 an = 0;
  u64* ids = JABCcfoldIds(&an, ctx, args[4], exception);
  u8* const dbase = u8bIdle(destb)[0];
  u8s into = {dbase, u8bIdle(destb)[1]};
  u64csc anc = {ids, ids + an};
  ok64 o = CFOLDMerge(into, &w, commit, anc);
  free(ids);
  if (o != OK) JABC_THROW("cfold.merge: failed (buffer full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - dbase));
}

//  _cfold_alive(blob, len, outIdle) -> bytes written (the last-folded view)
static JABC_FN(JABCcfoldAlive) {
  if (argc < 3) JABC_THROW("cfold._alive(blob, len, outIdle)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  u8* bb[4] = {};
  if (!JABCIdleOf(bb, ctx, args[2], exception)) JABC_UNDEF;
  if (CFOLDAlive(&w, bb) != OK) JABC_THROW("cfold.alive: output buffer full");
  return JSValueMakeNumber(ctx, (double)u8bDataLen(bb));
}

//  _cfold_produce(blob, len, revHashlet, outIdle) -> bytes written
//  The file as commit `rev` saw it; visibility is STORED, so no scope bitmap.
static JABC_FN(JABCcfoldProduce) {
  if (argc < 4) JABC_THROW("cfold._produce(blob, len, rev, outIdle)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  u32 rev = 0;
  if (!JABCcfoldRev(&rev, &w, ctx, args[2], exception))
    JABC_THROW("cfold.produce: no such commit");
  u8* bb[4] = {};
  if (!JABCIdleOf(bb, ctx, args[3], exception)) JABC_UNDEF;
  if (CFOLDProduce(&w, rev, bb, NULL) != OK)
    JABC_THROW("cfold.produce: output buffer full");
  return JSValueMakeNumber(ctx, (double)u8bDataLen(bb));
}

//  _cfold_blame(blob, len, off) -> the 16-char hashlet of the commit that
//  appended the token at body offset `off` (one range binary search).
static JABC_FN(JABCcfoldBlame) {
  if (argc < 3) JABC_THROW("cfold._blame(blob, len, off)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    JABC_THROW("cfold.blame: the weave is empty or malformed");
  u32 off = 0;
  if (!JABCu32Of(&off, ctx, args[2], exception)) JABC_UNDEF;
  u32 ci = 0;
  if (CFOLDBlame(&ci, &w, off) != OK)
    JABC_THROW("cfold.blame: that offset belongs to no commit");
  cfcommit c = {};
  if (CFOLDCommitAt(&c, &w, ci) != OK) JABC_THROW("cfold.blame: malformed weave");
  return JABCcfoldHashlet(ctx, c.id);
}

//  Set object property `k` = `v` (small helper for the token record).
static inline void JABCcfoldProp(JSContextRef ctx, JSObjectRef o, const char* k,
                                 JSValueRef v) {
  JSStringRef ks = JSStringCreateWithUTF8CString(k);
  JSObjectSetProperty(ctx, o, ks, v, kJSPropertyAttributeNone, NULL);
  JSStringRelease(ks);
}

//  _cfold_itermem(blob, len) -> the step cursor length, in u32 elements
static JABC_FN(JABCcfoldIterMem) {
  if (argc < 2) JABC_THROW("cfold._itermem(blob, len)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  return JSValueMakeNumber(ctx, (double)CFOLDIterMem(&w));
}

//  _cfold_step(blob, len, revHashlet, cursor) -> token record | false
//  One preorder-DFS step at `rev`.  `cursor` is a JS-owned Uint32Array of
//  _cfold_itermem elements, ZERO-FILLED before the first call — the DFS state
//  lives there and C stays stateless (the _weave_step pattern).  The record:
//  `text` (a subarray of the blob), `tag`, `off`/`end` (BODY offsets — the
//  identity, feed `off` to _cfold_blame), `alive`.
static JABC_FN(JABCcfoldStep) {
  if (argc < 4) JABC_THROW("cfold._step(blob, len, rev, cursor)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeBoolean(ctx, false);
  u32 rev = 0;
  if (!JABCcfoldRev(&rev, &w, ctx, args[2], exception))
    JABC_THROW("cfold.step: no such commit");
  u8* curb[4] = {};
  if (!JABCDataOf(curb, ctx, args[3], exception)) JABC_UNDEF;
  size_t nel = u8bDataLen(curb) / sizeof(u32);
  if (nel < CFOLDIterMem(&w))
    JABC_THROW("cfold.step: the cursor is too small (size it by _cfold_itermem)");
  u32* cur = (u32*)u8bData(curb)[0];
  u32s mem = {cur, cur + nel};
  cfoldtok t = {};
  b8 got = NO;
  if (CFOLDIterNext(&t, &got, &w, rev, mem) != OK)
    JABC_THROW("cfold.step: the weave is malformed");
  if (!got) return JSValueMakeBoolean(ctx, false);
  JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
  size_t toff = (size_t)(w.body[0] + t.off - (u8c*)base);
  JABCcfoldProp(ctx, o, "text",
                JABCSubU8(ctx, args[0], toff, (size_t)(t.end - t.off), exception));
  JABCcfoldProp(ctx, o, "tag", JSValueMakeNumber(ctx, (double)t.tag));
  JABCcfoldProp(ctx, o, "off", JSValueMakeNumber(ctx, (double)t.off));
  JABCcfoldProp(ctx, o, "end", JSValueMakeNumber(ctx, (double)t.end));
  JABCcfoldProp(ctx, o, "alive", JSValueMakeBoolean(ctx, t.alive));
  return o;
}

//  _cfold_emitdiff(blob, len, name, navver, from, to, hunkDest, hunkOff)
//  -> watermark.  Windowed diff from-rev -> to-rev (both COMMIT HASHLETS),
//  emitted as 'H' records (toks carry the per-token diff side) appended into
//  the HUNK container `hunkDest` at hunkOff.
static JABC_FN(JABCcfoldEmitDiff) {
  if (argc < 8) JABC_THROW("cfold._emitdiff(blob,len,name,navver,from,to,hunk,off)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    JABC_THROW("cfold.emitDiff: the weave is empty or malformed");
  u8 ntmp[FILE_PATH_MAX_LEN], vtmp[FILE_PATH_MAX_LEN];
  u8s name = {}, nav = {};
  if (!JABCArgU8(name, ctx, args[2], ntmp, sizeof(ntmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(nav, ctx, args[3], vtmp, sizeof(vtmp), exception)) return JSValueMakeUndefined(ctx);
  u32 from = 0, to = 0;
  if (!JABCcfoldRev(&from, &w, ctx, args[4], exception))
    JABC_THROW("cfold.emitDiff: no such commit (from)");
  if (!JABCcfoldRev(&to, &w, ctx, args[5], exception))
    JABC_THROW("cfold.emitDiff: no such commit (to)");
  u8* destb[4] = {};
  if (!JABCIdleOf(destb, ctx, args[6], exception)) JABC_UNDEF;
  if (!JABCBufFed(destb, ctx, args[7], exception)) JABC_UNDEF;  //  DATA = [0,off)
  u8cs namec = {name[0], name[1]}, navc = {nav[0], nav[1]};
  JABCemit em = {{u8bIdle(destb)[0], u8bIdle(destb)[1]}, OK};
  ok64 o = CFOLDEmitDiff(&w, namec, navc, from, to, JABCcfoldEmitCb, &em);
  if (o != OK || em.err != OK) JABC_THROW("cfold.emitDiff: failed (buffer full?)");
  return JSValueMakeNumber(
      ctx, (double)(u8bDataLen(destb) + (size_t)(em.into[0] - u8bIdle(destb)[0])));
}

//  _cfold_emitfull(blob, len, name, scheme, navver, from, to, hunkDest, hunkOff)
//  -> watermark.  Whole-file variant; `scheme` prefixes the emitted URIs.
static JABC_FN(JABCcfoldEmitFull) {
  if (argc < 9) JABC_THROW("cfold._emitfull(blob,len,name,scheme,navver,from,to,hunk,off)");
  cfold w = {};
  u8* base = NULL;
  if (!JABCcfoldAt(&w, &base, ctx, args[0], args[1], exception))
    JABC_THROW("cfold.emitFull: the weave is empty or malformed");
  u8 ntmp[FILE_PATH_MAX_LEN], stmp[FILE_PATH_MAX_LEN], vtmp[FILE_PATH_MAX_LEN];
  u8s name = {}, sch = {}, nav = {};
  if (!JABCArgU8(name, ctx, args[2], ntmp, sizeof(ntmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(sch, ctx, args[3], stmp, sizeof(stmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(nav, ctx, args[4], vtmp, sizeof(vtmp), exception)) return JSValueMakeUndefined(ctx);
  u32 from = 0, to = 0;
  if (!JABCcfoldRev(&from, &w, ctx, args[5], exception))
    JABC_THROW("cfold.emitFull: no such commit (from)");
  if (!JABCcfoldRev(&to, &w, ctx, args[6], exception))
    JABC_THROW("cfold.emitFull: no such commit (to)");
  u8* destb[4] = {};
  if (!JABCIdleOf(destb, ctx, args[7], exception)) JABC_UNDEF;
  if (!JABCBufFed(destb, ctx, args[8], exception)) JABC_UNDEF;  //  DATA = [0,off)
  u8cs namec = {name[0], name[1]}, schc = {sch[0], sch[1]}, navc = {nav[0], nav[1]};
  JABCemit em = {{u8bIdle(destb)[0], u8bIdle(destb)[1]}, OK};
  ok64 o = CFOLDEmitFull(&w, namec, schc, navc, from, to, JABCcfoldEmitCb, &em);
  if (o != OK || em.err != OK) JABC_THROW("cfold.emitFull: failed (buffer full?)");
  return JSValueMakeNumber(
      ctx, (double)(u8bDataLen(destb) + (size_t)(em.into[0] - u8bIdle(destb)[0])));
}

static inline void JABCCfoldInstall(JSObjectRef o) {
  JABC_API_FN(o, "_cfold_count", JABCcfoldCount);
  JABC_API_FN(o, "_cfold_commits", JABCcfoldCommits);
  JABC_API_FN(o, "_cfold_next", JABCcfoldNext);
  JABC_API_FN(o, "_cfold_merge", JABCcfoldMerge);
  JABC_API_FN(o, "_cfold_alive", JABCcfoldAlive);
  JABC_API_FN(o, "_cfold_produce", JABCcfoldProduce);
  JABC_API_FN(o, "_cfold_blame", JABCcfoldBlame);
  JABC_API_FN(o, "_cfold_itermem", JABCcfoldIterMem);
  JABC_API_FN(o, "_cfold_step", JABCcfoldStep);
  JABC_API_FN(o, "_cfold_emitdiff", JABCcfoldEmitDiff);
  JABC_API_FN(o, "_cfold_emitfull", JABCcfoldEmitFull);
}

#endif
