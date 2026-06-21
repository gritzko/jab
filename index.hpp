#ifndef JABC_INDEX_HPP
#define JABC_INDEX_HPP
//  INDEX bindings (JS-022): the 3 native leaves an `abc.index` LSM stands on.
//  An index is a stack of immutable, mmapped sorted runs (oldest-first) + a
//  memtable; the write/persist path is PURE JS over the existing HIT leaves
//  (sort / merge / book / close).  These three leaves are the READ + COMPACT
//  primitives, each pure marshalling over ONE abc/dog function — no format
//  logic, no held JS reference (rule #4).
//
//  Leaves (per lane):
//    _findge_<lane>(run, needleHi[, needleLo]) -> index of first elem >= needle
//        binds <lane>sFindGE (abc/Sx.h).  Point lookup: JS reads the element
//        at the returned index off the run's typed-array view and tests the
//        key bound itself (mirrors keeper KEEPLookup: FindGE then key-hi test).
//    _seekrange_<lane>(runs[], loHi[, loLo], hiHi[, hiLo], cb) -> undefined
//        binds HIT<lane>SeekRange (abc/HITx.h).  Trims a heap of run slices to
//        [lo, hi), then DRAINS it (HIT<lane>Step), streaming each hit through
//        the IN-FRAME cb (mirrors spot capo_seek_prefix + io.readdir(path,cb)).
//    _compact_<lane>(runs[], out) -> [mergedElems, m]
//        binds HIT<lane>Compact (abc/HITx.h).  Builds the oldest-first run
//        stack, runs the 1/8 size-tiered ladder (full-element dedup, newest
//        in the stream wins on a tie), writes the merged run into `out`.
//        Returns the merged element count + how many youngest runs (m) it
//        collapsed, so JS replaces runs[len-m .. len) with the one merged run.
//
//  Lanes: the two REAL shapes — wh128 (keeper puppy registry: (key,val), point
//  on key) and u64 (spot trigram: scalar).  u64 keys/needles/lo/hi cross as
//  BigInt; wh128 as (key,val) BigInt pairs.  kv64 is DEFERRED (its Z is key-
//  only, so compaction dedup ≠ full-element / newest-wins — keyed compaction,
//  out of scope).
//
//  IMPORTANT: the including TU pulls in hit.hpp first, which instantiates
//  <lane>sFindGE / sFindRange (Sx.h via QSORTx) and HIT<lane>* (HITx.h) for
//  every lane.  This header adds only the marshalling leaves on top.
#include "cont.hpp"

extern "C" {
#include "dog/WHIFF.h"  // wh128 (key,val) + wh128Z
}

//  Read one lane element (the needle / lo / hi) from JS args.  wh128 takes two
//  BigInt args (key, val); u64 one BigInt.  `i` is the first arg index.
static inline wh128 JABCIdxWh128(JSContextRef ctx, const JSValueRef args[],
                                 size_t i, JSValueRef* ex) {
  wh128 v;
  v.key = (u64)JSValueToUInt64(ctx, args[i], ex);
  v.val = (u64)JSValueToUInt64(ctx, args[i + 1], ex);
  return v;
}
static inline u64 JABCIdxU64(JSContextRef ctx, const JSValueRef args[], size_t i,
                             JSValueRef* ex) {
  return (u64)JSValueToUInt64(ctx, args[i], ex);
}

//  --- _findge_<lane>: binary-search a sorted run for the first elem >= needle.
//  Returns the index (0..count); JS reads the element + tests the bound.  ARGN
//  is how many JS args the needle spans (1 scalar / 2 pair); RDNEEDLE fills it.
#define FINDGE_LEAF(L, ARGN, RDNEEDLE)                                         \
  static JABC_FN(jfindge_##L) {                                               \
    if (argc < 1 + (ARGN)) JABC_THROW("_findge_" #L "(run, needle…)");        \
    void* base;                                                              \
    size_t cap;                                                             \
    if (!JABCLaneArr(&base, &cap, ctx, args[0], sizeof(L), exception))        \
      return JSValueMakeUndefined(ctx);                                       \
    L needle = RDNEEDLE;                                                      \
    if (*exception) return JSValueMakeUndefined(ctx);                         \
    L* bb = (L*)base;                                                         \
    L##cs run = {bb, bb + cap};                                              \
    L const* pos = L##sFindGE(run, &needle);                                  \
    return JSValueMakeNumber(ctx, (double)(size_t)(pos - bb));                \
  }

//  --- _seekrange_<lane>: heap of run slices -> [lo,hi) -> drain through cb.
//  cb is invoked per hit with the lane element (a BigInt, or a [key,val] pair);
//  its return is a stop signal: false / "enough" stops, truthy / undefined /
//  "more" continues, a throw aborts + propagates (mirror io.readdir(path,cb)).
//  EMIT(ctx, el) builds the JS value for one element pointer `el`.
#define SEEKRANGE_LEAF(L, ARGN, RDLO, RDHI, EMIT)                             \
  static JABC_FN(jseekrange_##L) {                                            \
    if (argc < 2 + 2 * (ARGN) || !JSValueIsObject(ctx, args[0]))             \
      JABC_THROW("_seekrange_" #L "(runs[], lo…, hi…, cb)");                  \
    JSObjectRef arr = (JSObjectRef)args[0];                                   \
    JSStringRef lk = JSStringCreateWithUTF8CString("length");                 \
    size_t N = (size_t)JSValueToNumber(                                       \
        ctx, JSObjectGetProperty(ctx, arr, lk, exception), exception);        \
    JSStringRelease(lk);                                                      \
    if (N > 64) JABC_THROW("_seekrange: too many runs (max 64)");             \
    L lo = RDLO, hi = RDHI;                                                   \
    if (*exception) return JSValueMakeUndefined(ctx);                         \
    JSValueRef cbv = args[1 + 2 * (ARGN)];                                    \
    if (!JSValueIsObject(ctx, cbv) ||                                        \
        !JSObjectIsFunction(ctx, (JSObjectRef)cbv))                          \
      JABC_THROW("_seekrange: cb must be a function");                        \
    JSObjectRef cb = (JSObjectRef)cbv;                                        \
    L##cs ent[64];                                                            \
    for (size_t i = 0; i < N; i++) {                                          \
      JSValueRef el =                                                         \
          JSObjectGetPropertyAtIndex(ctx, arr, (unsigned)i, exception);       \
      u8s b = {};                                                             \
      if (!JABCBytesOf(b, ctx, el, exception))                                \
        return JSValueMakeUndefined(ctx);                                     \
      ent[i][0] = (const L*)b[0];                                             \
      ent[i][1] = (const L*)b[1];                                             \
    }                                                                         \
    L##css heap = {ent, ent + N};                                            \
    HIT##L##SeekRange(heap, &lo, &hi);                                        \
    while (!$empty(heap)) {                                                   \
      L const* top = (*heap[0])[0];                                           \
      JSValueRef el = EMIT;                                                   \
      JSValueRef exc = NULL;                                                  \
      JSValueRef r = JSObjectCallAsFunction(ctx, cb, NULL, 1, &el, &exc);     \
      if (exc != NULL) {                                                      \
        *exception = exc;                                                     \
        return JSValueMakeUndefined(ctx);                                     \
      }                                                                       \
      if (!JSValueIsUndefined(ctx, r) && JSValueIsBoolean(ctx, r) &&          \
          !JSValueToBoolean(ctx, r))                                          \
        break; /* false -> stop */                                           \
      if (JSValueIsString(ctx, r)) {                                          \
        char tag[8] = "";                                                     \
        JSStringRef s = JSValueToStringCopy(ctx, r, NULL);                    \
        JSStringGetUTF8CString(s, tag, sizeof(tag));                          \
        JSStringRelease(s);                                                   \
        if (strcmp(tag, "enough") == 0) break;                                \
      }                                                                       \
      HIT##L##Step(heap);                                                     \
    }                                                                         \
    return JSValueMakeUndefined(ctx);                                         \
  }

//  --- _compact_<lane>: the 1/8 ladder.  `runs[]` is the oldest-first stack of
//  live run slices; `out` is a destination container sized to >= sum(runs).
//  Runs the merge, returns [mergedElems, m] (m youngest runs collapsed).
#define COMPACT_LEAF(L)                                                       \
  static JABC_FN(jcompact_##L) {                                              \
    if (argc < 2 || !JSValueIsObject(ctx, args[0]))                          \
      JABC_THROW("_compact_" #L "(runs[], out)");                            \
    JSObjectRef arr = (JSObjectRef)args[0];                                   \
    JSStringRef lk = JSStringCreateWithUTF8CString("length");                 \
    size_t N = (size_t)JSValueToNumber(                                       \
        ctx, JSObjectGetProperty(ctx, arr, lk, exception), exception);        \
    JSStringRelease(lk);                                                      \
    if (N > 64) JABC_THROW("_compact: too many runs (max 64)");               \
    L##cs ent[64];                                                            \
    for (size_t i = 0; i < N; i++) {                                          \
      JSValueRef el =                                                         \
          JSObjectGetPropertyAtIndex(ctx, arr, (unsigned)i, exception);       \
      u8s b = {};                                                             \
      if (!JABCBytesOf(b, ctx, el, exception))                                \
        return JSValueMakeUndefined(ctx);                                     \
      ent[i][0] = (const L*)b[0];                                             \
      ent[i][1] = (const L*)b[1];                                             \
    }                                                                         \
    u8s d = {};                                                               \
    if (!JABCBytesOf(d, ctx, args[1], exception))                             \
      return JSValueMakeUndefined(ctx);                                       \
    L* base = (L*)d[0];                                                       \
    L##s into = {base, (L*)d[1]};                                            \
    L##css stack = {ent, ent + N};                                           \
    size_t before = $len(stack);                                             \
    if (HIT##L##Compact(stack, into) != OK)                                  \
      JABC_THROW("_compact: out too small");                                  \
    size_t m = before - $len(stack) + 1;                                     \
    size_t merged = (size_t)((*into) - base);                                 \
    if (m < 2) merged = 0; /* nothing collapsed; out is untouched */         \
    return JABCPair(ctx, JSValueMakeNumber(ctx, (double)merged),              \
                    JSValueMakeNumber(ctx, (double)m));                       \
  }

//  --- wh128: (key,val); needle/lo/hi span 2 BigInt args.  Emit a [key,val]
//  pair; point/range/prefix all order by (key,val).
FINDGE_LEAF(wh128, 2, JABCIdxWh128(ctx, args, 1, exception))
SEEKRANGE_LEAF(wh128, 2, JABCIdxWh128(ctx, args, 1, exception),
               JABCIdxWh128(ctx, args, 3, exception),
               JABCPair(ctx, JSBigIntCreateWithUInt64(ctx, top->key, exception),
                        JSBigIntCreateWithUInt64(ctx, top->val, exception)))
COMPACT_LEAF(wh128)

//  --- u64: scalar; needle/lo/hi each one BigInt.  Emit a BigInt.
FINDGE_LEAF(u64, 1, JABCIdxU64(ctx, args, 1, exception))
SEEKRANGE_LEAF(u64, 1, JABCIdxU64(ctx, args, 1, exception),
               JABCIdxU64(ctx, args, 2, exception),
               JSBigIntCreateWithUInt64(ctx, *top, exception))
COMPACT_LEAF(u64)

#define INDEX_REG(L)                                                  \
  JABC_API_FN(o, "_findge_" #L, jfindge_##L);                         \
  JABC_API_FN(o, "_seekrange_" #L, jseekrange_##L);                   \
  JABC_API_FN(o, "_compact_" #L, jcompact_##L)

static inline void JABCIndexInstall(JSObjectRef o) {
  INDEX_REG(wh128);
  INDEX_REG(u64);
}

#endif
