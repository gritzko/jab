#ifndef ABC_JS_H
#define ABC_JS_H
#include <stdlib.h>

//  Include ABC C headers OUTSIDE the namespace first so system types
//  (struct timespec, etc.) land in the global namespace; ABC.hpp then
//  re-includes them (guarded) inside namespace abc.
extern "C" {
#include "abc/ABC.h"
}

#include "JavaScriptCore/JSBase.h"
#include "JavaScriptCore/JSContextRef.h"
#include "JavaScriptCore/JSObjectRef.h"
#include "JavaScriptCore/JSStringRef.h"
#include "JavaScriptCore/JSTypedArray.h"
#include "JavaScriptCore/JSValueRef.h"
#include "abc/ABC.hpp"

using namespace abc;

extern thread_local JSGlobalContextRef JABC_CONTEXT;
extern thread_local JSObjectRef JABC_GLOBAL_OBJECT;

//  JAB-008: PRO.h's ABC_BASS (C-style __thread TLS, see pro.hpp); declared
//  here so the guard below needs no PRO.h include (§6 keeps it out of headers).
extern "C" {
extern __thread u8* ABC_BASS[4];
}

//  JAB-008: the binding-side equivalent of PRO.h call()'s BASS bracket — dup
//  the DATA+IDLE heads, restore on every exit path (JABC_THROW / C++ unwind).
class JABCBassGuard {
  u8* data_;
  u8* idle_;

 public:
  JABCBassGuard() : data_(ABC_BASS[1]), idle_(ABC_BASS[2]) {}
  ~JABCBassGuard() {
    ABC_BASS[1] = data_;
    ABC_BASS[2] = idle_;
  }
};

//  JAB-008: run a binding leaf under the guard, so raw C-op invocations from
//  JABC_FNs leak no BASS carve set (main maps ABC_BASS once, no call() rewinds).
template <JSObjectCallAsFunctionCallback F>
JSValueRef JABCBassGuarded(JSContextRef ctx, JSObjectRef function,
                           JSObjectRef self, size_t argc,
                           const JSValueRef args[], JSValueRef* exception) {
  JABCBassGuard bass;
  return F(ctx, function, self, argc, args, exception);
}

//  A native binding function.  Its return value IS the JS result; on error
//  it sets *exception (via JABC_THROW) and returns undefined.
#define JABC_FN(fn)                                                       \
  JSValueRef fn(JSContextRef ctx, JSObjectRef function, JSObjectRef self, \
                size_t argc, const JSValueRef args[], JSValueRef* exception)

#define JABC_UNDEF return JSValueMakeUndefined(ctx)

//  Throw a JS Error with `msg` and return undefined.
#define JABC_THROW(msg)                                  \
  do {                                                   \
    JSStringRef m_ = JSStringCreateWithUTF8CString(msg); \
    JSValueRef v_ = JSValueMakeString(ctx, m_);          \
    *exception = JSObjectMakeError(ctx, 1, &v_, NULL);   \
    JSStringRelease(m_);                                 \
    return JSValueMakeUndefined(ctx);                    \
  } while (0)

//  Register a fresh API object `o` as a global of the same name.
#define JABC_API_OBJECT(o)                                           \
  JSObjectRef o = JSObjectMake(JABC_CONTEXT, NULL, NULL);            \
  {                                                                  \
    JSStringRef n_ = JSStringCreateWithUTF8CString(#o);              \
    JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, n_, o,     \
                        kJSPropertyAttributeNone, NULL);             \
    JSStringRelease(n_);                                             \
  }

//  Attach native function `f` as method `n` of object `o`.  JAB-008: every
//  leaf is registered through the BASS bracket (JABCBassGuarded above).
#define JABC_API_FN(o, n, f)                                                  \
  {                                                                           \
    JSStringRef fn_ = JSStringCreateWithUTF8CString(n);                       \
    JSObjectSetProperty(JABC_CONTEXT, o, fn_,                                 \
                        JSObjectMakeFunctionWithCallback(JABC_CONTEXT, fn_,   \
                                                         JABCBassGuarded<f>), \
                        kJSPropertyAttributeNone, NULL);                      \
    JSStringRelease(fn_);                                                     \
  }

//  Make a JS string value from a C string (used for error text / small keys).
JSValueRef JSOfCString(const char* str);

//  Read a typed array's backing range into a binary slice.  Sets *exception
//  and returns NO on a non-typed-array / detached (NULL ptr) argument.
b8 JABCBytesOf(u8s out, JSContextRef ctx, JSValueRef arg, JSValueRef* exception);

//  JS-108: THE u8-slice -> JS string conversion — length-explicit (embedded
//  NULs survive, never truncates); invalid UTF-8 -> U+FFFD; throws on OOM.
JSValueRef JABCStrOfSlice(JSContextRef ctx, u8cs s, JSValueRef* exception);

void JABCExecute(const char* script);
void JABCReport(JSValueRef exception);

ok64 JABCutf8Install();
ok64 JABCioInstall();
ok64 JABCbufInstall();
ok64 JABCContInstall();
ok64 JABCTokInstall();
ok64 JABCUriInstall();
ok64 JABCCodecInstall();
ok64 JABCZipInstall();
ok64 JABCAnsiInstall();
ok64 JABCConsoleInstall();
ok64 JABCRequireInstall();
ok64 JABCPolInstall();
ok64 JABCPolUninstall();
ok64 JABCNetInstall();
ok64 JABCTtyInstall();
ok64 JABCioUninstall();

#endif
