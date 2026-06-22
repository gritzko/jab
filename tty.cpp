#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "JABC.hpp"
extern "C" {
#include "abc/ANSI.h"
}

//  Terminal-control leaves (JS-053) over abc/ANSI's ANSI* POSIX wrappers
//  (next to ANSIBgColor, which shares the raw-mode dance).
//  Distinct from ansi.* (styling); this is terminal CONTROL.
//  STATELESS like every JABC leaf (rule #4): tty.raw RETURNS the saved termios
//  as a fresh Uint8Array that JS holds; tty.cook takes those bytes back to
//  restore.  No C-side per-fd table.  The future bin/bro.js pager owns the
//  saved-state buffer and the cook-on-exit safety (a try/finally + main's
//  uncaught hook), so a crash never wedges the user's shell in raw mode.

static int JABCttyInt(JSContextRef ctx, JSValueRef v, JSValueRef* exception) {
  return (int)JSValueToNumber(ctx, v, exception);
}

//  Set a numeric property on an object (the {rows, cols} record).
static void JABCttySetNum(JSContextRef ctx, JSObjectRef o, const char* k,
                          double v) {
  JSStringRef ks = JSStringCreateWithUTF8CString(k);
  JSObjectSetProperty(ctx, o, ks, JSValueMakeNumber(ctx, v),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(ks);
}

//  tty.raw(fd) -> Uint8Array  (the saved termios; pass it to tty.cook to
//  restore).  Enters raw mode (BRO.c flags: ECHO|ICANON|ISIG|IEXTEN,
//  IXON|ICRNL|BRKINT|INPCK|ISTRIP, OPOST cleared; VMIN=0 VTIME=1).
static JABC_FN(JABCttyRaw) {
  if (argc < 1) JABC_THROW("tty.raw(fd) -> savedTermios");
  int fd = JABCttyInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  size_t n = ANSITtyTermiosSize();
  JSObjectRef ta =
      JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, n, exception);
  if (*exception || ta == NULL) return JSValueMakeUndefined(ctx);
  u8* p = (u8*)JSObjectGetTypedArrayBytesPtr(ctx, ta, exception);
  if (*exception || p == NULL) return JSValueMakeUndefined(ctx);
  u8s saved = {p, p + n};
  if (ANSIRaw(fd, saved) != OK) JABC_THROW(strerror(errno));
  return ta;
}

//  tty.cook(fd, saved) -> restore termios from the bytes tty.raw returned.
static JABC_FN(JABCttyCook) {
  if (argc < 2) JABC_THROW("tty.cook(fd, savedTermios)");
  int fd = JABCttyInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  u8s saved = {};
  if (!JABCBytesOf(saved, ctx, args[1], exception))
    return JSValueMakeUndefined(ctx);
  if ((size_t)$len(saved) != ANSITtyTermiosSize())
    JABC_THROW("tty.cook: saved termios has wrong size");
  u8cs in = {saved[0], saved[1]};
  if (ANSICook(fd, in) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  tty.size(fd?) -> {rows, cols}  (ioctl TIOCGWINSZ; default fd is stdout).
static JABC_FN(JABCttySize) {
  int fd = STDOUT_FILENO;
  if (argc > 0 && !JSValueIsUndefined(ctx, args[0])) {
    fd = JABCttyInt(ctx, args[0], exception);
    if (*exception) return JSValueMakeUndefined(ctx);
  }
  u16 rows = 0, cols = 0;
  if (ANSITtySize(fd, &rows, &cols) != OK) JABC_THROW(strerror(errno));
  JSObjectRef obj = JSObjectMake(ctx, NULL, NULL);
  JABCttySetNum(ctx, obj, "rows", (double)rows);
  JABCttySetNum(ctx, obj, "cols", (double)cols);
  return obj;
}

//  tty.openpty() -> {master, slave}  (test support: a fresh pty pair; the
//  caller owns + io.close()s both fds).  Lets the JS test set raw on a real
//  slave tty under ctest, where no controlling terminal exists.
static JABC_FN(JABCttyOpenPty) {
  (void)args;
  (void)argc;
  int master = -1, slave = -1;
  if (ANSIOpenPty(&master, &slave) != OK) JABC_THROW(strerror(errno));
  JSObjectRef obj = JSObjectMake(ctx, NULL, NULL);
  JABCttySetNum(ctx, obj, "master", (double)master);
  JABCttySetNum(ctx, obj, "slave", (double)slave);
  return obj;
}

//  tty.setSize(fd, rows, cols) -> ioctl TIOCSWINSZ (test support: stamp a
//  winsize on a pty so tty.size can read it back).
static JABC_FN(JABCttySetSize) {
  if (argc < 3) JABC_THROW("tty.setSize(fd, rows, cols)");
  int fd = JABCttyInt(ctx, args[0], exception);
  double rows = JSValueToNumber(ctx, args[1], exception);
  double cols = JSValueToNumber(ctx, args[2], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (ANSISetSize(fd, (u16)rows, (u16)cols) != OK)
    JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

ok64 JABCTtyInstall() {
  JABC_API_OBJECT(tty);
  JABC_API_FN(tty, "raw", JABCttyRaw);
  JABC_API_FN(tty, "cook", JABCttyCook);
  JABC_API_FN(tty, "size", JABCttySize);
  JABC_API_FN(tty, "openpty", JABCttyOpenPty);
  JABC_API_FN(tty, "setSize", JABCttySetSize);
  return OK;
}
