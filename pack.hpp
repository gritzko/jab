#ifndef JABC_PACK_HPP
#define JABC_PACK_HPP
//  PACK — an OFFSET-ADDRESSED git pack log (header + [obj-hdr][zlib] records)
//  in a u8 buffer.  Reads/writes are byte offsets only; sha addressing is the
//  index's job (a wh128 layer above), so this binding never takes a sha.
//
//  GIT-007: PURE marshalling over the dog/git pack-log core.  Every entry
//  resolves a typed array to a u8s (JABCBytesOf) and calls ONE dog/git PACK
//  function — zero delta/encode/inflate decisions live here.  The writer is
//  PACKu8sFeedObj (GIT-002), the reader PACKResolveOfs (GIT-004); both take
//  caller-provided scratch, so this binding never malloc()s.
//
//  write: _pack_header(buf,off,count)->end ; _pack_feed(buf,off,type,content,
//         base,baseOff,delta)->end  (base non-empty + baseOff>=0 → OFS_DELTA,
//         else raw — the dog/git writer decides)
//  read:  _pack_next(buf,off,dataLen)->objEnd|-1 ;
//         _pack_resolve(buf,recOff,base,delta) -> resolved bytes (Uint8Array) ;
//         _pack_type/_size/_baseoff/_ref/_inflate(buf,recOff,...)
#include "cont.hpp"

extern "C" {
#include "abc/HEX.h"
#include "dog/git/DELT.h"
#include "dog/git/GIT.h"
#include "dog/git/PACK.h"
#include "dog/git/PIDX.h"
}

//  A fresh lowercase-hex JS string over `n` raw bytes (sha shorthand).
static inline JSValueRef JABCHexOf(JSContextRef ctx, const u8* bin, size_t n) {
  char* h = (char*)malloc(n * 2 + 1);
  if (!h) return JSValueMakeUndefined(ctx);
  u8s hx = {(u8*)h, (u8*)h + n * 2};
  u8cs b = {(u8*)bin, (u8*)bin + n};
  HEXu8sFeedSome(hx, b);
  h[n * 2] = 0;
  JSStringRef s = JSStringCreateWithUTF8CString(h);
  free(h);
  JSValueRef v = JSValueMakeString(ctx, s);
  JSStringRelease(s);
  return v;
}

//  Set object property `name` to value `v` (small helper for record objects).
static inline void JABCSet(JSContextRef ctx, JSObjectRef o, const char* name,
                           JSValueRef v) {
  JSStringRef n = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(ctx, o, n, v, kJSPropertyAttributeNone, NULL);
  JSStringRelease(n);
}

//  _pack_header(buf, off, count) -> off+12
static JABC_FN(JABCpackHeader) {
  if (argc < 3) JABC_THROW("pack._header(buf, off, count)");
  u8s c = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[1], exception);
  u32 count = (u32)JSValueToNumber(ctx, args[2], exception);
  u8s into = {c[0] + off, c[1]};
  if (PACKu8sFeedHdr(into, count) != OK) JABC_THROW("pack: header");
  return JSValueMakeNumber(ctx, (double)(size_t)(into[0] - c[0]));
}

//  _pack_count(buf) -> object count (from the 12-byte header)
static JABC_FN(JABCpackCount) {
  if (argc < 1) JABC_THROW("pack._count(buf)");
  u8s c = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  u8cs from = {c[0], c[1]};
  pack_hdr hdr = {};
  if (PACKDrainHdr(from, &hdr) != OK) JABC_THROW("pack: bad header");
  return JSValueMakeNumber(ctx, (double)hdr.count);
}

//  _pack_feed(buf, off, type, content, base, baseOff, delta) -> new write head
//  GIT-007: pure marshalling — resolve the typed arrays and hand the
//  raw|OFS_DELTA decision + emit to the dog/git writer PACKu8sFeedObj, which
//  checks every header/varint/deflate return (no ignored-return double-header).
static JABC_FN(JABCpackFeed) {
  if (argc < 4) JABC_THROW("pack._feed(buf, off, type, content, base, baseOff, delta)");
  u8s c = {}, content = {}, base = {}, delta = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[1], exception);
  u8 type = (u8)JSValueToNumber(ctx, args[2], exception);
  if (!JABCBytesOf(content, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  //  base (resolved bytes) + baseOff are optional: absent/empty → raw record.
  if (argc > 4) JABCBytesOf(base, ctx, args[4], exception);
  double bod = argc > 5 ? JSValueToNumber(ctx, args[5], exception) : -1;
  if (argc > 6) JABCBytesOf(delta, ctx, args[6], exception);

  u8* b[4] = {c[0], c[0] + off, c[0] + off, c[1]};  //  log: DATA ends at off
  u8csc bc = {base[0], base[1]};
  u8csc cc = {content[0], content[1]};
  u8* d[4] = {delta[0], delta[0], delta[0], delta[1]};  //  delta encode scratch
  u64 base_off = bod >= 0 ? (u64)bod : (u64)off;  //  off when no base (no delta)
  if (PACKu8sFeedObj((u8bp)b, type, cc, bc, (u64)off, base_off, (u8bp)d, NULL) != OK)
    JABC_THROW("pack: feed (full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(b[2] - c[0]));
}

//  Drain the object header at recOff (advancing nothing the caller sees).
static b8 JABCpackAt(pack_obj* obj, u8s c, JSContextRef ctx, JSValueRef bufv,
                     JSValueRef offv, JSValueRef* ex) {
  if (!JABCBytesOf(c, ctx, bufv, ex)) return NO;
  size_t rec = (size_t)JSValueToNumber(ctx, offv, ex);
  u8cs from = {c[0] + rec, c[1]};
  return PACKDrainObjHdr(from, obj) == OK;
}

//  _pack_next(buf, off, dataLen) -> end of the object at off | -1
//  GIT-007: a zlib stream isn't length-delimited, so ask the dog/git
//  resolver where the record ends — no JS-side re-inflate-to-measure.
static JABC_FN(JABCpackNext) {
  if (argc < 3) JABC_THROW("pack._next(buf, off, dataLen)");
  u8s c = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[1], exception);
  size_t dl = (size_t)JSValueToNumber(ctx, args[2], exception);
  if (off >= dl) return JSValueMakeNumber(ctx, -1);
  u8cs pack = {c[0], c[0] + dl};
  u64 end = 0;
  if (PACKRecordEnd(pack, (u64)off, &end) != OK) return JSValueMakeNumber(ctx, -1);
  return JSValueMakeNumber(ctx, (double)(size_t)end);
}

static JABC_FN(JABCpackType) {
  u8s c = {};
  pack_obj o = {};
  if (argc < 2 || !JABCpackAt(&o, c, ctx, args[0], args[1], exception))
    return JSValueMakeUndefined(ctx);
  return JSValueMakeNumber(ctx, (double)o.type);
}
static JABC_FN(JABCpackSize) {
  u8s c = {};
  pack_obj o = {};
  if (argc < 2 || !JABCpackAt(&o, c, ctx, args[0], args[1], exception))
    return JSValueMakeUndefined(ctx);
  return JSValueMakeNumber(ctx, (double)o.size);
}
static JABC_FN(JABCpackBaseOff) {
  u8s c = {};
  pack_obj o = {};
  if (argc < 2 || !JABCpackAt(&o, c, ctx, args[0], args[1], exception))
    return JSValueMakeUndefined(ctx);
  if (o.type != PACK_OBJ_OFS_DELTA) return JSValueMakeNumber(ctx, -1);
  size_t rec = (size_t)JSValueToNumber(ctx, args[1], exception);
  return JSValueMakeNumber(ctx, (double)(rec - o.ofs_delta));
}
static JABC_FN(JABCpackRef) {
  u8s c = {};
  pack_obj o = {};
  if (argc < 2 || !JABCpackAt(&o, c, ctx, args[0], args[1], exception))
    return JSValueMakeUndefined(ctx);
  if (o.type != PACK_OBJ_REF_DELTA) return JSValueMakeUndefined(ctx);
  return JABCBlob(ctx, o.ref_delta[0], 20);
}

//  _pack_inflate(buf, recOff, out, outOff) -> bytes inflated (= obj.size)
//  Raw single-record read: drain the header, inflate the one zlib stream.
static JABC_FN(JABCpackInflate) {
  if (argc < 4) JABC_THROW("pack._inflate(buf, recOff, out, outOff)");
  u8s c = {};
  pack_obj o = {};
  if (!JABCpackAt(&o, c, ctx, args[0], args[1], exception))
    JABC_THROW("pack: bad record");
  size_t rec = (size_t)JSValueToNumber(ctx, args[1], exception);
  u8s out = {};
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  size_t oo = (size_t)JSValueToNumber(ctx, args[3], exception);
  u8cs from = {c[0] + rec, c[1]};
  pack_obj tmp = {};
  PACKDrainObjHdr(from, &tmp);
  u8s into = {out[0] + oo, out[1]};
  if (PACKInflate(from, into, tmp.size) != OK) JABC_THROW("pack: inflate (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)tmp.size);
}

//  _pack_resolve(buf, recOff, base, delta) -> resolved object bytes
//  GIT-007: the WHOLE delta chase is the dog/git resolver PACKResolveOfs;
//  base/delta are caller-owned scratch, the result is copied out to a fresh
//  Uint8Array (the chase aliasing stays inside the scratch, never leaks).
static JABC_FN(JABCpackResolve) {
  if (argc < 4) JABC_THROW("pack._resolve(buf, recOff, base, delta)");
  u8s c = {}, base = {}, delta = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t rec = (size_t)JSValueToNumber(ctx, args[1], exception);
  if (!JABCBytesOf(base, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(delta, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  u8cs pack = {c[0], c[1]};
  u8cs out = {};
  u8 type = 0;
  //  JS-055: surface NOROOM distinctly (the wrapper grows scratch); REF_DELTA
  //  stays its own loud fail so detection isn't lost to a grow loop.
  ok64 r = PACKResolveOfs(pack, (u64)rec, base, delta, out, &type);
  if (r == NOROOM) JABC_THROW("pack: resolve NOROOM");
  if (r == PACKREF) JABC_THROW("pack: resolve ref-delta");
  if (r != OK) JABC_THROW("pack: resolve");
  return JABCBlob(ctx, out[0], (size_t)u8csLen(out));
}

//  _delt_apply(base, delta, out, outOff) -> reconstructed bytes
static JABC_FN(JABCdeltApply) {
  if (argc < 4) JABC_THROW("delt._apply(base, delta, out, outOff)");
  u8s base = {}, delta = {}, out = {};
  if (!JABCBytesOf(base, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(delta, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  size_t oo = (size_t)JSValueToNumber(ctx, args[3], exception);
  u8cs dl = {delta[0], delta[1]};
  u8cs bl = {base[0], base[1]};
  u8* op = out[0] + oo;
  u8* og[3] = {op, op, out[1]};
  if (DELTApply(dl, bl, og) != OK) JABC_THROW("delt: apply (out full?)");
  return JSValueMakeNumber(ctx, (double)(size_t)(og[1] - op));
}

//  JS-036: _delt_encode(base, target, out, outOff) -> n delta bytes appended
//  at out[outOff], or -1 on DELTFAIL (delta not smaller than target -> raw).
static JABC_FN(JABCdeltEncode) {
  if (argc < 4) JABC_THROW("delt._encode(base, target, out, outOff)");
  u8s base = {}, target = {}, out = {};
  if (!JABCBytesOf(base, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(target, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  size_t oo = (size_t)JSValueToNumber(ctx, args[3], exception);
  u8* o[4] = {out[0], out[0] + oo, out[0] + oo, out[1]};  //  DATA ends at outOff
  u8csc b = {base[0], base[1]}, t = {target[0], target[1]};
  ok64 r = DELTEncode(b, t, (u8bp)o);
  if (r == DELTFAIL) return JSValueMakeNumber(ctx, -1);
  if (r != OK) JABC_THROW("delt: encode");
  return JSValueMakeNumber(ctx, (double)(size_t)(o[2] - (out[0] + oo)));
}

//  _pack_scan(buf, dataLen, out, base, delta) -> entry count
//  GIT-010: pure marshalling over the dog/git scan-emit PIDXScan.  Walk the
//  whole pack [0, dataLen), resolve+git-sha each object, and drop one wh128
//  `(key=hashlet60|type, val=offset)` entry per object STRAIGHT into the
//  caller's region `out` (a Buf's IDLE), returning the entry count.  base /
//  delta are caller-owned resolve scratch (PACKResolveOfs's sizing).  Guards,
//  ALL before any write: out must be 8-byte aligned (a wh128 u64 view needs
//  it; a fresh/reset Buf IDLE head is 8-aligned) and hold count*16 bytes
//  worst case.  A REF_DELTA (no sha-addressed base in an OFS-only log) makes
//  PIDXScan fail -> throw, so a partial scan never half-fills a reused buffer.
//  JS-055: NOROOM (an inflated object exceeds the scratch) is surfaced as a
//  DISTINCT throw so the wrapper grows scratch instead of guessing ref-delta.
static JABC_FN(JABCpackScan) {
  if (argc < 5) JABC_THROW("pack._scan(buf, dataLen, out, base, delta)");
  u8s c = {}, out = {}, base = {}, delta = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t dl = (size_t)JSValueToNumber(ctx, args[1], exception);
  if (!JABCBytesOf(out, ctx, args[2], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(base, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  if (!JABCBytesOf(delta, ctx, args[4], exception)) return JSValueMakeUndefined(ctx);
  u8cs pack = {c[0], c[0] + dl};
  pack_hdr hdr = {};
  u8cs hv = {pack[0], pack[1]};   //  PACKDrainHdr CONSUMES — validate on a copy
  if (PACKDrainHdr(hv, &hdr) != OK) JABC_THROW("pack.scan: bad header");
  if (((uintptr_t)out[0] & 7u) != 0)
    JABC_THROW("pack.scan: out not 8-byte aligned (reset the Buf)");
  size_t need = (size_t)hdr.count * sizeof(wh128);
  if ((size_t)$len(out) < need) JABC_THROW("pack.scan: out too small");
  //  Bwh128 over the caller's region: emit lands at [out[0], out[1]).
  wh128* wb = (wh128*)out[0];
  wh128* wcap = (wh128*)(out[0] + (((size_t)$len(out)) / sizeof(wh128)) * sizeof(wh128));
  wh128* wbuf[4] = {wb, wb, wb, wcap};
  ok64 r = PIDXScan(pack, 0, wbuf, base, delta);  //  PACK-001: 0 = whole pack
  if (r == NOROOM) JABC_THROW("pack.scan: NOROOM");
  if (r == PACKREF) JABC_THROW("pack.scan: ref-delta");
  if (r != OK) JABC_THROW("pack.scan: scan (out full? corrupt?)");
  size_t n = (size_t)(wbuf[2] - wb);   //  emitted entries (DATA grew by n)
  return JSValueMakeNumber(ctx, (double)n);
}

//  _pack_feed_emit(type, content, offset, out, outOff) -> 16 (bytes written)
//  GIT-010: the index-on-append twin — git-sha the content the caller JUST
//  fed (no resolve) and write ONE wh128 entry at out[outOff].  Mirrors
//  PIDXFeedEmit; out is the caller's wh128 entry sink (a Buf's IDLE).
static JABC_FN(JABCpackFeedEmit) {
  if (argc < 5) JABC_THROW("pack._feed_emit(type, content, offset, out, outOff)");
  u8 type = (u8)JSValueToNumber(ctx, args[0], exception);
  u8s content = {}, out = {};
  if (!JABCBytesOf(content, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  u64 offset = (u64)JSValueToNumber(ctx, args[2], exception);
  if (!JABCBytesOf(out, ctx, args[3], exception)) return JSValueMakeUndefined(ctx);
  size_t oo = (size_t)JSValueToNumber(ctx, args[4], exception);
  u8* slot = out[0] + oo;
  if (((uintptr_t)slot & 7u) != 0)
    JABC_THROW("pack.feedEmit: out slot not 8-byte aligned");
  if ((size_t)(out[1] - slot) < sizeof(wh128)) JABC_THROW("pack.feedEmit: out full");
  wh128* wbuf[4] = {(wh128*)slot, (wh128*)slot, (wh128*)slot, (wh128*)slot + 1};
  u8csc cc = {content[0], content[1]};
  if (PIDXFeedEmit(wbuf, type, cc, offset) != OK)
    JABC_THROW("pack.feedEmit: emit");
  return JSValueMakeNumber(ctx, (double)sizeof(wh128));
}

//  _git_tree_next(bytes, off) -> {mode, nameStart, nameEnd, sha, nextOff} | null
//  JS-028: ONE pure-marshalling drain of a single tree entry via the dog/git
//  parser GITu8sDrainTree (tree format `(<mode> <name>\0<20-byte sha>)*`).  The
//  binding holds nothing: it re-slices the caller's typed array from `off`,
//  drains one entry, and reports the name span (positions into `bytes`, for a
//  zero-copy subarray in JS), the parsed octal mode (incl. 0o160000 gitlinks),
//  the 40-hex sha, and the next read offset.  At end-of-tree -> null (the JS
//  cursor's .next() returns false).  GITu8sFileSplit peels "<mode> " off the
//  "<mode> <name>" head so `name` is the bare name span — no JS framing.
static JABC_FN(JABCgitTreeNext) {
  if (argc < 2) JABC_THROW("git._tree_next(bytes, off)");
  u8s c = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  size_t off = (size_t)JSValueToNumber(ctx, args[1], exception);
  if (off >= (size_t)$len(c)) return JSValueMakeNull(ctx);
  u8cs obj = {c[0] + off, c[1]};
  u8cs file = {}, sha1 = {};
  u32 mode = 0;
  ok64 r = GITu8sDrainTree(obj, file, sha1, &mode);
  if (r == NODATA) return JSValueMakeNull(ctx);
  if (r != OK) JABC_THROW("git.tree: bad tree entry");
  //  Split "<mode> <name>" -> bare name span (positions into the source bytes).
  u8cs name = {};
  GITu8sFileSplit(file, NULL, name);
  JSObjectRef o = JSObjectMake(ctx, NULL, NULL);
  JABCSet(ctx, o, "mode", JSValueMakeNumber(ctx, (double)mode));
  JABCSet(ctx, o, "nameStart", JSValueMakeNumber(ctx, (double)(size_t)(name[0] - c[0])));
  JABCSet(ctx, o, "nameEnd", JSValueMakeNumber(ctx, (double)(size_t)(name[1] - c[0])));
  JABCSet(ctx, o, "sha", JABCHexOf(ctx, sha1[0], GIT_SHA1_LEN));
  JABCSet(ctx, o, "nextOff", JSValueMakeNumber(ctx, (double)(size_t)(obj[0] - c[0])));
  return o;
}

//  _git_parse_commit(bytes) -> {tree, parents[], foster[], author, committer, body}
//  JS-028: eager — commit objects are small.  Drives the dog/git header
//  iterator GITu8sDrainCommit directly (the only way to surface parents/foster/
//  committer/body, which GITu8sParseCommit's git_commit struct doesn't carry),
//  plus GITu8sCommitTree for the tree sha.  `tree`/`parents`/`foster` are
//  40-hex strings; `author`/`committer`/`body` are UTF-8 decoded values.  No
//  manual git framing in JS — every split is a dog/git drain.
static JABC_FN(JABCgitParseCommit) {
  if (argc < 1) JABC_THROW("git._parse_commit(bytes)");
  u8s c = {};
  if (!JABCBytesOf(c, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);

  JSObjectRef o = JSObjectMake(ctx, NULL, NULL);

  //  tree sha (binary -> hex); empty string when absent/malformed.
  u8cs commit = {c[0], c[1]};
  u8 tree_sha[GIT_SHA1_LEN] = {};
  if (GITu8sCommitTree(commit, tree_sha) == OK)
    JABCSet(ctx, o, "tree", JABCHexOf(ctx, tree_sha, GIT_SHA1_LEN));
  else
    JABCSet(ctx, o, "tree", JSOfCString(""));  // JS-109: releases its ref

  //  Walk the headers: collect parents/foster (hex sha values), pick the last
  //  author/committer ident lines; the blank line yields the body.
  JSObjectRef parents = JSObjectMakeArray(ctx, 0, NULL, NULL);
  JSObjectRef foster = JSObjectMakeArray(ctx, 0, NULL, NULL);
  JSValueRef author = JSValueMakeUndefined(ctx);
  JSValueRef committer = JSValueMakeUndefined(ctx);
  JSValueRef body = JSOfCString("");  // JS-109: releases its ref
  unsigned np = 0, nf = 0;

  u8cs scan = {c[0], c[1]};
  u8cs field = {}, value = {};
  while (GITu8sDrainCommit(scan, field, value) == OK) {
    if (field[0] == field[1]) {  //  blank line -> body is the rest
      body = JABCStrOfSlice(ctx, value, exception);
      break;
    }
    JSValueRef hv = JABCStrOfSlice(ctx, value, exception);
    if (u8csEq(field, GIT_FIELD_PARENT))
      JSObjectSetPropertyAtIndex(ctx, parents, np++, hv, NULL);
    else if (u8csEq(field, GIT_FIELD_FOSTER))
      JSObjectSetPropertyAtIndex(ctx, foster, nf++, hv, NULL);
    else if (u8csEq(field, GIT_FIELD_AUTHOR))
      author = hv;
    else if (u8csEq(field, GIT_FIELD_COMMITTER))
      committer = hv;
  }

  JABCSet(ctx, o, "parents", parents);
  JABCSet(ctx, o, "foster", foster);
  JABCSet(ctx, o, "author", author);
  JABCSet(ctx, o, "committer", committer);
  JABCSet(ctx, o, "body", body);
  return o;
}

static inline void JABCPackInstall(JSObjectRef o) {
  JABC_API_FN(o, "_git_tree_next", JABCgitTreeNext);
  JABC_API_FN(o, "_git_parse_commit", JABCgitParseCommit);
  JABC_API_FN(o, "_pack_header", JABCpackHeader);
  JABC_API_FN(o, "_pack_count", JABCpackCount);
  JABC_API_FN(o, "_pack_feed", JABCpackFeed);
  JABC_API_FN(o, "_pack_next", JABCpackNext);
  JABC_API_FN(o, "_pack_type", JABCpackType);
  JABC_API_FN(o, "_pack_size", JABCpackSize);
  JABC_API_FN(o, "_pack_baseoff", JABCpackBaseOff);
  JABC_API_FN(o, "_pack_ref", JABCpackRef);
  JABC_API_FN(o, "_pack_inflate", JABCpackInflate);
  JABC_API_FN(o, "_pack_resolve", JABCpackResolve);
  JABC_API_FN(o, "_pack_scan", JABCpackScan);
  JABC_API_FN(o, "_pack_feed_emit", JABCpackFeedEmit);
  JABC_API_FN(o, "_delt_apply", JABCdeltApply);
  JABC_API_FN(o, "_delt_encode", JABCdeltEncode);
}

#endif
