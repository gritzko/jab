//  arg.cpp — PTR-010: THE JS->C argument boundary.  A JS number is untrusted
//  input, never a size_t: `(size_t)JSValueToNumber(...)` turns the `-1` that
//  git.pack leaves in `_rec` after a failed seek into SIZE_MAX, and the
//  `{c[0] + off, c[1]}` that follows starts a slice one byte BELOW the
//  mapping with its head past its term — no callee bounds check can see it.
//  Every conversion goes through the gates here; the arithmetic is abc's.
#include "JABC.hpp"

//  --- JS object properties (the JABCSet/JABCGet pair, shared) -------------

JSValueRef JABCGetProp(JSContextRef ctx, JSObjectRef o, const char* name) {
  JSStringRef n = JSStringCreateWithUTF8CString(name);
  JSValueRef v = JSObjectGetProperty(ctx, o, n, NULL);
  JSStringRelease(n);
  return v;
}

void JABCSetProp(JSContextRef ctx, JSObjectRef o, const char* name,
                 JSValueRef v) {
  JSStringRef n = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(ctx, o, n, v, kJSPropertyAttributeNone, NULL);
  JSStringRelease(n);
}

//  --- number gates: the only JSValueToNumber callers in the bindings -------

//  A JS number -> u64.  Rejects NaN (also a missing argument), +-Inf,
//  negative, fractional and anything past 2^53-1, where a double stops
//  counting integers exactly.  The value dies here, while it still prints
//  as itself and not as 18446744073709551615.
b8 JABCu64Of(u64* out, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  double d = JSValueToNumber(ctx, arg, ex);
  if (*ex) return NO;
  if (!(d >= 0) || d > 9007199254740991.0 || d != (double)(u64)d) {
    *ex = JSOfCString("expected a whole non-negative number");
    return NO;
  }
  *out = (u64)d;
  return YES;
}

//  Where a NEGATIVE sentinel is part of the contract (pack's baseOff = -1
//  means "no base").  Still rejects NaN, +-Inf and fractions — a sentinel is
//  one specific value, not a licence to skip the gate.
b8 JABCi64Of(i64* out, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  double d = JSValueToNumber(ctx, arg, ex);
  if (*ex) return NO;
  if (!(d >= -9007199254740991.0 && d <= 9007199254740991.0) ||
      d != (double)(i64)d) {
    *ex = JSOfCString("expected a whole number");
    return NO;
  }
  *out = (i64)d;
  return YES;
}

b8 JABCu32Of(u32* out, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  u64 v = 0;
  if (!JABCu64Of(&v, ctx, arg, ex)) return NO;
  if (v > 0xffffffffUL) {
    *ex = JSOfCString("number does not fit 32 bits");
    return NO;
  }
  *out = (u32)v;
  return YES;
}

b8 JABCu8Of(u8* out, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  u64 v = 0;
  if (!JABCu64Of(&v, ctx, arg, ex)) return NO;
  if (v > 0xffUL) {
    *ex = JSOfCString("number does not fit a byte");
    return NO;
  }
  *out = (u8)v;
  return YES;
}

//  --- buffers: what actually crosses the boundary -------------------------

//  Shared typed-array unwrap: the VIEW's range (BytesPtr is the ArrayBuffer
//  base, not the view's start — a subarray shares the buffer, so a view with
//  byteOffset > 0 would otherwise read the wrong bytes).
static b8 JABCViewOf(u8** base, size_t* len, JSContextRef ctx, JSValueRef arg,
                     JSValueRef* ex) {
  if (JSValueGetTypedArrayType(ctx, arg, NULL) == kJSTypedArrayTypeNone) {
    *ex = JSOfCString("expected a typed array");
    return NO;
  }
  JSObjectRef obj = JSValueToObject(ctx, arg, ex);
  if (*ex) return NO;
  u8* p = (u8*)JSObjectGetTypedArrayBytesPtr(ctx, obj, ex);
  if (*ex) return NO;
  size_t n = JSObjectGetTypedArrayByteLength(ctx, obj, ex);
  if (*ex) return NO;
  //  A detached/neutered ArrayBuffer yields a NULL bytes ptr (len may be 0).
  if (p == NULL && n != 0) {
    *ex = JSOfCString("detached buffer");
    return NO;
  }
  size_t off = JSObjectGetTypedArrayByteOffset(ctx, obj, ex);
  if (*ex) return NO;
  *base = p + off;
  *len = n;
  return YES;
}

//  A read source: the whole view is DATA, IDLE empty.  Read it with
//  u8bDataC(buf) and walk it with JABCBufAt — never with a pointer.
b8 JABCDataOf(u8b buf, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  u8* base = NULL;
  size_t len = 0;
  if (!JABCViewOf(&base, &len, ctx, arg, ex)) return NO;
  u8** b = (u8**)buf;                    //  the bMap recipe: creators cast
  b[0] = b[1] = base;
  b[2] = b[3] = base + len;
  if (!u8bOK(buf)) {
    *ex = JSOfCString("bad buffer bounds");
    return NO;
  }
  return YES;
}

//  A write target: the whole view is IDLE, DATA empty.  Fill it through
//  u8bIdle(buf) / u8bFeed; the bytes produced are u8bDataLen(buf), never a
//  pointer subtraction at the call site.
b8 JABCIdleOf(u8b buf, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  u8* base = NULL;
  size_t len = 0;
  if (!JABCViewOf(&base, &len, ctx, arg, ex)) return NO;
  u8** b = (u8**)buf;                    //  the bMap recipe: creators cast
  b[0] = b[1] = b[2] = base;
  b[3] = base + len;
  if (!u8bOK(buf)) {
    *ex = JSOfCString("bad buffer bounds");
    return NO;
  }
  return YES;
}

//  A JS-given position INSIDE a slice: the number gate + a bounds check
//  (== length is the empty tail, legal).  Errors read in plain words.
b8 JABCOffOf(size_t* out, u8csc whole, JSContextRef ctx, JSValueRef arg,
             JSValueRef* ex) {
  u64 v = 0;
  if (!JABCu64Of(&v, ctx, arg, ex)) return NO;
  if (v > (u64)u8csLen(whole)) {
    *ex = JSOfCString("offset is past the end of the buffer");
    return NO;
  }
  *out = (size_t)v;
  return YES;
}

//  Position a read source's DATA at a JS-given offset: the gate above plus
//  u8bUsed, which returns MISS past the border.  The consumed prefix becomes
//  PAST, so the callee that needs the WHOLE buffer (an OFS-delta chase reads
//  backwards) still has it — u8bDataC for the record, u8bcs for the log.
b8 JABCBufAt(u8b buf, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  size_t off = 0;
  if (!JABCOffOf(&off, u8bDataC(buf), ctx, arg, ex)) return NO;
  if (u8bUsed(buf, off) != OK) {
    *ex = JSOfCString("offset is past the end of the buffer");
    return NO;
  }
  return YES;
}

//  The write-side twin of JABCBufAt: place a write target's DATA/IDLE
//  boundary at a JS-given offset, so the callee fills u8bIdle(buf) from
//  there and the bytes it produced are u8bDataLen(buf).
b8 JABCBufFed(u8b buf, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  size_t off = 0;
  if (!JABCOffOf(&off, u8bIdleC(buf), ctx, arg, ex)) return NO;
  if (u8bFed(buf, off) != OK) {
    *ex = JSOfCString("offset is past the end of the buffer");
    return NO;
  }
  return YES;
}

//  A JS Buf object ({bytes, _data, _idle} — buf.cpp) as a real u8b, cursor
//  and all.  The two cursors are JS numbers: gated, then checked against
//  each other and the view (PAST <= DATA <= IDLE <= end) before any read.
b8 JABCBufOf(u8b buf, JSContextRef ctx, JSValueRef arg, JSValueRef* ex) {
  if (!JSValueIsObject(ctx, arg)) {
    *ex = JSOfCString("expected a Buf");
    return NO;
  }
  JSObjectRef bo = JSValueToObject(ctx, arg, ex);
  if (*ex) return NO;
  if (!JABCIdleOf(buf, ctx, JABCGetProp(ctx, bo, "bytes"), ex)) return NO;
  u64 data = 0, idle = 0;
  if (!JABCu64Of(&data, ctx, JABCGetProp(ctx, bo, "_data"), ex)) return NO;
  if (!JABCu64Of(&idle, ctx, JABCGetProp(ctx, bo, "_idle"), ex)) return NO;
  //  IDLE first (it is the outer bound), then DATA inside what is left.
  if (data > idle || u8bFed(buf, (size_t)idle) != OK ||
      u8bUsed(buf, (size_t)data) != OK) {
    *ex = JSOfCString("the buffer's cursor is out of range");
    return NO;
  }
  return YES;
}

//  Hand the advanced cursors back to the JS Buf — a failed run still ate
//  what it ate, so callers write back on every exit path.
void JABCBufBack(JSContextRef ctx, JSObjectRef bo, u8b buf) {
  JABCSetProp(ctx, bo, "_data", JSValueMakeNumber(ctx, (double)u8bPastLen(buf)));
  JABCSetProp(ctx, bo, "_idle", JSValueMakeNumber(ctx, (double)u8bBusyLen(buf)));
}
