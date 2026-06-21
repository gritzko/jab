#ifndef JABC_WEAVE_HPP
#define JABC_WEAVE_HPP
//  WEAVE — one file's whole DAG history as a 'W' TLV blob (columnar weave).
//  A WEAVE container is a JS-owned u8 buffer holding ONE 'W' blob; the binding
//  parses it zero-copy per call (stateless leaves, like HUNK).  The builders
//  (_weave_next/_weave_merge) write a FRESH 'W' blob into a target buffer.
//
//  Hashes are STRINGS (JABC convention): a commit id is the hi64 of the commit
//  sha1, presented as a 16-char hex hashlet.  Every u64<->hex conversion lives
//  here; no u64 ever crosses the boundary as a JS number/BigInt.  Small values
//  that fit a double (commit INDEX, tag, counts) stay plain numbers.
//
//  hunk.hpp must precede this header (shared JABCBytesOf / JABCArgU8 / JABCSubU8).
#include "cont.hpp"

extern "C" {
#include "dog/WEAVE.h"
}

//  --- u64 <-> 16-char hex hashlet (big-endian: first sha byte = top bits) ---
//  A commit id is be64(sha1[0..8]); its 16 hex chars are the value's hex.
static inline u64 JABCweaveHi64(JSContextRef ctx, JSValueRef v, JSValueRef* ex) {
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
static inline JSValueRef JABCweaveHashlet(JSContextRef ctx, u64 h) {
  static const char HX[] = "0123456789abcdef";
  char b[17];
  for (int i = 15; i >= 0; i--) { b[i] = HX[h & 0xf]; h >>= 4; }
  b[16] = 0;
  JSStringRef s = JSStringCreateWithUTF8CString(b);
  JSValueRef v = JSValueMakeString(ctx, s);
  JSStringRelease(s);
  return v;
}

//  Parse the 'W' blob in (args[bi], args[li]=byte length) into `w`, also
//  reporting the blob base.  NO on a non-array or a malformed/empty blob.
static inline bool JABCweaveAt(weave* w, u8** base, JSContextRef ctx,
                               JSValueRef bv, JSValueRef lv, JSValueRef* ex) {
  u8s blob = {};
  if (!JABCBytesOf(blob, ctx, bv, ex)) return false;
  size_t len = (size_t)JSValueToNumber(ctx, lv, ex);
  if (len > (size_t)$len(blob)) len = (size_t)$len(blob);
  *base = blob[0];
  u8csc bc = {blob[0], blob[0] + len};
  return WEAVEParse(w, bc) == OK;
}

//  Set object property `k` = `v` (small helper for the token record).
static inline void JABCweaveProp(JSContextRef ctx, JSObjectRef o, const char* k,
                                 JSValueRef v) {
  JSStringRef ks = JSStringCreateWithUTF8CString(k);
  JSObjectSetProperty(ctx, o, ks, v, kJSPropertyAttributeNone, NULL);
  JSStringRelease(ks);
}

//  Emit sink: append each emitted hunk as a TLV 'H' record into a HUNK
//  container's buffer (`into` advances per record).  The JABC rule #4 — C holds
//  no JS closure — holds: the callback is this C leaf, never a JS function.
typedef struct { u8s into; ok64 err; } JABCemit;
static ok64 JABCweaveEmitCb(hunkc* hk, void* vctx) {
  JABCemit* c = (JABCemit*)vctx;
  ok64 o = HUNKu8sFeed(c->into, hk);
  if (o != OK) c->err = o;
  return o;
}

//  _weave_count(blob, len) -> token count (0 for empty/unbuilt)
static JABC_FN(JABCweaveCount) {
  if (argc < 2) JABC_THROW("weave._count(blob, len)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  return JSValueMakeNumber(ctx, (double)(u32)$len(w.toks));
}

//  _weave_commits(blob, len) -> Array of 16-char hashlet strings (commits[0]=spine)
static JABC_FN(JABCweaveCommits) {
  if (argc < 2) JABC_THROW("weave._commits(blob, len)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSObjectMakeArray(ctx, 0, NULL, exception);
  u32 n = (u32)$len(w.commits);
  if (n == 0) return JSObjectMakeArray(ctx, 0, NULL, exception);
  JSValueRef* els = (JSValueRef*)malloc(n * sizeof(JSValueRef));
  if (els == NULL) JABC_THROW("weave.commits: oom");
  for (u32 i = 0; i < n; i++) els[i] = JABCweaveHashlet(ctx, w.commits[0][i]);
  JSObjectRef arr = JSObjectMakeArray(ctx, n, els, exception);
  free(els);
  return arr;
}

//  _weave_step(blob, len, cursor) -> token record | false
//  cursor is a JS-owned Uint32Array(6): consumed byte offsets into
//  [toks, text, ins, rms, anc] plus the running token-end `off`.  The leaf
//  rebuilds a working `weave` from those offsets, steps once (WEAVEStep), and
//  writes the advanced offsets back — the cursor lives in JS, C is stateless.
static JABC_FN(JABCweaveStep) {
  if (argc < 3) JABC_THROW("weave._step(blob, len, cursor)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeBoolean(ctx, false);
  u8s cur = {};
  if (!JABCBytesOf(cur, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  if ((size_t)$len(cur) < 24) JABC_THROW("weave._step: cursor needs 24 bytes");
  u32* C = (u32*)cur[0];
  weave c = w;
  if (w.toks[0]) c.toks[0] = (tok32c*)((u8c*)w.toks[0] + C[0]);
  if (w.text[0]) c.text[0] = w.text[0] + C[1];
  if (w.ins[0])  c.ins[0]  = w.ins[0]  + C[2];
  if (w.rms[0])  c.rms[0]  = w.rms[0]  + C[3];
  if (w.anc[0])  c.anc[0]  = w.anc[0]  + C[4];
  u32 off = C[5];
  weavetok tk = {};
  if (WEAVEStep(&c, &off, &tk) != OK) return JSValueMakeBoolean(ctx, false);
  C[0] = w.toks[0] ? (u32)((u8c*)c.toks[0] - (u8c*)w.toks[0]) : 0;
  C[1] = w.text[0] ? (u32)(c.text[0] - w.text[0]) : 0;
  C[2] = w.ins[0]  ? (u32)(c.ins[0]  - w.ins[0])  : 0;
  C[3] = w.rms[0]  ? (u32)(c.rms[0]  - w.rms[0])  : 0;
  C[4] = w.anc[0]  ? (u32)(c.anc[0]  - w.anc[0])  : 0;
  C[5] = off;
  JSObjectRef t = JSObjectMake(ctx, NULL, NULL);
  size_t toff = (size_t)((u8c*)tk.text[0] - base);
  JABCweaveProp(ctx, t, "text",
                JABCSubU8(ctx, args[0], toff, (size_t)$len(tk.text), exception));
  JABCweaveProp(ctx, t, "tag", JSValueMakeNumber(ctx, (double)tk.tag));
  JABCweaveProp(ctx, t, "hasIn", JSValueMakeBoolean(ctx, tk.has_in));
  JABCweaveProp(ctx, t, "inserter", JSValueMakeNumber(ctx, (double)tk.inserter));
  size_t rn = (size_t)$len(tk.rms);
  JSObjectRef rms =
      JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint32Array, rn, exception);
  if (rn) {
    void* rp = JSObjectGetTypedArrayBytesPtr(ctx, rms, exception);
    if (rp) memcpy(rp, tk.rms[0], rn * sizeof(u32));
  }
  JABCweaveProp(ctx, t, "rms", rms);
  JABCweaveProp(ctx, t, "anchor", JABCweaveHashlet(ctx, tk.anchor));
  JABCweaveProp(ctx, t, "hasAnchor", JSValueMakeBoolean(ctx, tk.has_anchor));
  return t;
}

//  _weave_next(dest, base|null, baseLen, newBlob, ext, hash) -> blob byte length
//  Folds `newBlob` (tokenized by `ext`) onto `base` under commit `hash`,
//  writing a fresh 'W' blob from offset 0 of `dest`.  base NULL/empty => from-blob.
static JABC_FN(JABCweaveNext) {
  if (argc < 6) JABC_THROW("weave._next(dest, base, baseLen, newBlob, ext, hash)");
  u8s dest = {};
  if (!JABCBytesOf(dest, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  weave bw = {};
  weave* wp = NULL;
  u8* bbase = NULL;
  if (!JSValueIsNull(ctx, args[1]) && !JSValueIsUndefined(ctx, args[1])) {
    if (JABCweaveAt(&bw, &bbase, ctx, args[1], args[2], exception)) wp = &bw;
  }
  u8s nb = {};
  if (!JABCBytesOf(nb, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  u8 exttmp[64];
  u8s ext = {};
  if (!JABCArgU8(ext, ctx, args[4], exttmp, sizeof(exttmp), exception))
    return JSValueMakeUndefined(ctx);
  u64 commit = JABCweaveHi64(ctx, args[5], exception);
  u8s into = {dest[0], dest[1]};
  u8csc nbc = {nb[0], nb[1]};
  u8csc extc = {ext[0], ext[1]};
  if (WEAVENext(into, wp, nbc, extc, commit) != OK)
    JABC_THROW("weave.fold: failed (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - dest[0]));
}

//  _weave_merge(dest, aBlob, aLen, bBlob, bLen, hash) -> blob byte length
static JABC_FN(JABCweaveMerge) {
  if (argc < 6) JABC_THROW("weave._merge(dest, a, aLen, b, bLen, hash)");
  u8s dest = {};
  if (!JABCBytesOf(dest, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  weave aw = {}, bw = {};
  u8 *abase = NULL, *bbase = NULL;
  if (!JABCweaveAt(&aw, &abase, ctx, args[1], args[2], exception))
    JABC_THROW("weave.merge: bad a");
  if (!JABCweaveAt(&bw, &bbase, ctx, args[3], args[4], exception))
    JABC_THROW("weave.merge: bad b");
  u64 commit = JABCweaveHi64(ctx, args[5], exception);
  u8s into = {dest[0], dest[1]};
  if (WEAVEMerge(into, &aw, &bw, commit) != OK)
    JABC_THROW("weave.merge: failed (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - dest[0]));
}

//  _weave_idhash(hash, ordinal) -> 16-char hashlet string
static JABC_FN(JABCweaveIdHash) {
  if (argc < 2) JABC_THROW("weave._idhash(hash, ordinal)");
  u64 cid = JABCweaveHi64(ctx, args[0], exception);
  u32 ord = (u32)JSValueToNumber(ctx, args[1], exception);
  return JABCweaveHashlet(ctx, WEAVEIdHash(cid, ord));
}

//  _weave_scope(blob, len, activeHashlets[]) -> Uint8Array bitmap (u64-word sized)
//  bit i set <=> commits[i] reachable; bit 0 (spine) always set.
static JABC_FN(JABCweaveScope) {
  if (argc < 3) JABC_THROW("weave._scope(blob, len, active)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, 8, exception);
  u32 ncommits = (u32)$len(w.commits);
  u32 nwords = (ncommits + 63) >> 6;
  if (nwords == 0) nwords = 1;
  JSObjectRef bm = JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array,
                                          (size_t)nwords * 8, exception);
  if (*exception || bm == NULL) return JSValueMakeUndefined(ctx);
  void* bp = JSObjectGetTypedArrayBytesPtr(ctx, bm, exception);
  if (bp == NULL) return JSValueMakeUndefined(ctx);
  //  Parse the active list (a JS array of hashlet strings) into scratch u64s.
  JSObjectRef act = JSValueToObject(ctx, args[2], exception);
  u32 an = 0;
  if (act) {
    JSStringRef ls = JSStringCreateWithUTF8CString("length");
    an = (u32)JSValueToNumber(ctx, JSObjectGetProperty(ctx, act, ls, exception), exception);
    JSStringRelease(ls);
  }
  u64* ids = an ? (u64*)malloc(an * sizeof(u64)) : NULL;
  if (an && ids == NULL) JABC_THROW("weave.scope: oom");
  for (u32 i = 0; i < an; i++)
    ids[i] = JABCweaveHi64(ctx, JSObjectGetPropertyAtIndex(ctx, act, i, exception), exception);
  u64cs active = {ids, ids + an};
  u64* bb[4] = {(u64*)bp, (u64*)bp, (u64*)bp + nwords, (u64*)bp + nwords};
  ok64 o = WEAVEScope(bb, &w, active);
  free(ids);
  if (o != OK) JABC_THROW("weave.scope: failed");
  return bm;
}

//  _weave_alive(blob, len, outIdle) -> bytes written into outIdle (tip rm-clear)
static JABC_FN(JABCweaveAlive) {
  if (argc < 3) JABC_THROW("weave._alive(blob, len, outIdle)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  u8s out = {};
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  u8* bb[4] = {out[0], out[0], out[0], out[1]};
  if (WEAVEAlive(&w, bb) != OK) JABC_THROW("weave.alive: out full");
  return JSValueMakeNumber(ctx, (double)(size_t)(bb[2] - out[0]));
}

//  _weave_produce(blob, len, scopeBitmap, outIdle) -> bytes written (rev view)
static JABC_FN(JABCweaveProduce) {
  if (argc < 4) JABC_THROW("weave._produce(blob, len, scope, outIdle)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  u8s sc = {};
  if (!JABCBytesOf(sc, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  u8s out = {};
  if (!JABCBytesOf(out, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  u64cs scope = {(u64c*)sc[0], (u64c*)sc[1]};
  u8* bb[4] = {out[0], out[0], out[0], out[1]};
  if (WEAVEProduce(&w, scope, bb) != OK) JABC_THROW("weave.produce: out full");
  return JSValueMakeNumber(ctx, (double)(size_t)(bb[2] - out[0]));
}

//  _weave_emitdiff(blob, len, name, navver, from, to, hunkDest, hunkOff) -> watermark
//  Windowed diff from-scope -> to-scope, emitted as 'H' records (toks carry the
//  per-token diff side) appended into the HUNK container `hunkDest` at hunkOff.
static JABC_FN(JABCweaveEmitDiff) {
  if (argc < 8) JABC_THROW("weave._emitdiff(blob,len,name,navver,from,to,hunk,off)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    JABC_THROW("weave.emitDiff: bad weave");
  u8 ntmp[FILE_PATH_MAX_LEN], vtmp[FILE_PATH_MAX_LEN];
  u8s name = {}, nav = {}, fb = {}, tb = {}, dest = {};
  if (!JABCArgU8(name, ctx, args[2], ntmp, sizeof(ntmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(nav, ctx, args[3], vtmp, sizeof(vtmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(fb, ctx, args[4], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(tb, ctx, args[5], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(dest, ctx, args[6], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[7], exception);
  u64cs from = {(u64c*)fb[0], (u64c*)fb[1]}, to = {(u64c*)tb[0], (u64c*)tb[1]};
  u8cs namec = {name[0], name[1]}, navc = {nav[0], nav[1]};
  JABCemit em = {{dest[0] + off, dest[1]}, OK};
  ok64 o = WEAVEEmitDiff(&w, namec, navc, from, to, JABCweaveEmitCb, &em);
  if (o != OK || em.err != OK) JABC_THROW("weave.emitDiff: failed (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(em.into[0] - dest[0]));
}

//  _weave_emitfull(blob, len, name, scheme, navver, from, to, hunkDest, hunkOff) -> watermark
static JABC_FN(JABCweaveEmitFull) {
  if (argc < 9) JABC_THROW("weave._emitfull(blob,len,name,scheme,navver,from,to,hunk,off)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    JABC_THROW("weave.emitFull: bad weave");
  u8 ntmp[FILE_PATH_MAX_LEN], stmp[FILE_PATH_MAX_LEN], vtmp[FILE_PATH_MAX_LEN];
  u8s name = {}, sch = {}, nav = {}, fb = {}, tb = {}, dest = {};
  if (!JABCArgU8(name, ctx, args[2], ntmp, sizeof(ntmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(sch, ctx, args[3], stmp, sizeof(stmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCArgU8(nav, ctx, args[4], vtmp, sizeof(vtmp), exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(fb, ctx, args[5], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(tb, ctx, args[6], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(dest, ctx, args[7], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[8], exception);
  u64cs from = {(u64c*)fb[0], (u64c*)fb[1]}, to = {(u64c*)tb[0], (u64c*)tb[1]};
  u8cs namec = {name[0], name[1]}, schc = {sch[0], sch[1]}, navc = {nav[0], nav[1]};
  JABCemit em = {{dest[0] + off, dest[1]}, OK};
  ok64 o = WEAVEEmitFull(&w, namec, schc, navc, from, to, JABCweaveEmitCb, &em);
  if (o != OK || em.err != OK) JABC_THROW("weave.emitFull: failed (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(em.into[0] - dest[0]));
}

//  _weave_merged(blob, len, groups[], outIdle) -> bytes written
//  N-way conflict render: `groups` is a JS array of scope bitmaps (one per
//  merge side, <=32).  Shared runs emit once; divergent runs merge inline when
//  compatible, else frame as <<<< side1 |||| side2 >>>> (render-time only).
static JABC_FN(JABCweaveMerged) {
  if (argc < 4) JABC_THROW("weave._merged(blob, len, groups, outIdle)");
  weave w = {};
  u8* base = NULL;
  if (!JABCweaveAt(&w, &base, ctx, args[0], args[1], exception))
    return JSValueMakeNumber(ctx, 0);
  JSObjectRef ga = JSValueToObject(ctx, args[2], exception);
  if (ga == NULL) JABC_THROW("weave.merged: groups must be an array");
  JSStringRef ls = JSStringCreateWithUTF8CString("length");
  u32 ng = (u32)JSValueToNumber(ctx, JSObjectGetProperty(ctx, ga, ls, exception), exception);
  JSStringRelease(ls);
  if (ng > 32) JABC_THROW("weave.merged: at most 32 groups");
  weavescope groups[32];
  for (u32 i = 0; i < ng; i++) {
    u8s gb = {};
    if (!JABCBytesOf(gb, ctx, JSObjectGetPropertyAtIndex(ctx, ga, i, exception), exception))
      return JSValueMakeUndefined(ctx);
    groups[i][0] = (u64c*)gb[0];
    groups[i][1] = (u64c*)gb[1];
  }
  u8s out = {};
  if (!JABCBytesOf(out, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  u8* bb[4] = {out[0], out[0], out[0], out[1]};
  if (WEAVEEmitMerged(&w, (weavescope const*)groups, ng, bb) != OK)
    JABC_THROW("weave.merged: failed (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(bb[2] - out[0]));
}

static inline void JABCWeaveInstall(JSObjectRef o) {
  JABC_API_FN(o, "_weave_count", JABCweaveCount);
  JABC_API_FN(o, "_weave_commits", JABCweaveCommits);
  JABC_API_FN(o, "_weave_step", JABCweaveStep);
  JABC_API_FN(o, "_weave_next", JABCweaveNext);
  JABC_API_FN(o, "_weave_merge", JABCweaveMerge);
  JABC_API_FN(o, "_weave_idhash", JABCweaveIdHash);
  JABC_API_FN(o, "_weave_scope", JABCweaveScope);
  JABC_API_FN(o, "_weave_alive", JABCweaveAlive);
  JABC_API_FN(o, "_weave_produce", JABCweaveProduce);
  JABC_API_FN(o, "_weave_emitdiff", JABCweaveEmitDiff);
  JABC_API_FN(o, "_weave_emitfull", JABCweaveEmitFull);
  JABC_API_FN(o, "_weave_merged", JABCweaveMerged);
}

#endif
