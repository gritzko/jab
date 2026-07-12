#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "JABC.hpp"
#include "abc/FILE.h"
#include "abc/PATH.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

//  fds are plain numbers; buffers are JS-owned typed arrays.  These are the
//  leaf syscalls — one native call == one tight C operation over a fd and/or
//  a typed array's backing memory.  The cursor logic lives in the JS `Buf`
//  class (buf.js); nothing here holds memory or a JS reference.

static int JABCInt(JSContextRef ctx, JSValueRef v, JSValueRef* exception) {
  return (int)JSValueToNumber(ctx, v, exception);
}

//  Copy a JS-string path argument into a NUL-terminated path buffer.
static ok64 JABCPath(path8b path, JSContextRef ctx, JSValueRef arg,
                     JSValueRef* exception) {
  if (!JSValueIsString(ctx, arg)) return BADARG;
  JSStringRef s = JSValueToStringCopy(ctx, arg, exception);
  if (*exception || s == NULL) return BADARG;
  u8 page[PAGESIZE];
  size_t len = JSStringGetUTF8CString(s, (char*)page, PAGESIZE);
  JSStringRelease(s);
  if (len < 1 || len >= PAGESIZE) return NOROOM;  //  len counts the NUL
  u8cs src = {page, page + (len - 1)};
  ok64 o = u8bFeed(path, src);
  if (o != OK) return o;
  PATHu8bTerm(path);
  return OK;
}

//  io.open(path, "r"|"rw"|"c") -> fd
static JABC_FN(JABCioOpen) {
  if (argc < 1) JABC_THROW("io.open(path, mode) -> fd");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.open(): bad path");
  }
  char mode[8] = "r";
  if (argc > 1 && JSValueIsString(ctx, args[1])) {
    JSStringRef s = JSValueToStringCopy(ctx, args[1], NULL);
    JSStringGetUTF8CString(s, mode, sizeof(mode));
    JSStringRelease(s);
  }
  int fd = -1;
  ok64 o;
  if (strcmp(mode, "c") == 0) {
    //  FILECreate sets a sane 0600 mode; FILEOpen passes no mode to open(2),
    //  so an O_CREAT through it would get garbage perms.
    o = FILECreate(&fd, $path(path));
  } else {
    int flags = (strcmp(mode, "rw") == 0) ? O_RDWR : O_RDONLY;
    o = FILEOpen(&fd, $path(path), flags);
  }
  if (o != OK || fd < 0) JABC_THROW(strerror(errno));
  return JSValueMakeNumber(ctx, (double)fd);
}

//  io.close(fd)
static JABC_FN(JABCioClose) {
  if (argc < 1) JABC_THROW("io.close(fd)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  FILEClose(&fd);
  JABC_UNDEF;
}

//  io.sync(fd)
static JABC_FN(JABCioSync) {
  if (argc < 1) JABC_THROW("io.sync(fd)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (FILESync(&fd) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.size(fd) -> bytes
static JABC_FN(JABCioSize) {
  if (argc < 1) JABC_THROW("io.size(fd)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  size_t sz = 0;
  if (FILESize(&sz, &fd) != OK) JABC_THROW(strerror(errno));
  return JSValueMakeNumber(ctx, (double)sz);
}

//  io.resize(fd, n)
static JABC_FN(JABCioResize) {
  if (argc < 2) JABC_THROW("io.resize(fd, n)");
  int fd = JABCInt(ctx, args[0], exception);
  double n = JSValueToNumber(ctx, args[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (n < 0) JABC_THROW("io.resize(): negative size");
  if (FILEResize(&fd, (size_t)n) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.lock(fd, exclusive?) / io.unlock(fd)
static JABC_FN(JABCioLock) {
  if (argc < 1) JABC_THROW("io.lock(fd, exclusive)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  b8 excl = argc > 1 ? JSValueToBoolean(ctx, args[1]) : YES;
  if (FILELock(&fd, excl) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}
static JABC_FN(JABCioUnlock) {
  if (argc < 1) JABC_THROW("io.unlock(fd)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (FILEUnlock(&fd) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  Marshal a filled `filestat` into a JS {size, mode, kind, mtime, atime}
//  object.  size/mode are plain numbers; kind is a tag string; mtime/atime
//  are ron60 timestamps and cross as BigInt — the same encoding ron.encode /
//  ron.date / the JS dateCol consume (JS-021).  Shared by io.stat / io.lstat.
static JSObjectRef JABCStatObj(JSContextRef ctx, const filestat* fs) {
  JSObjectRef obj = JSObjectMake(ctx, NULL, NULL);
  JSStringRef k;
  k = JSStringCreateWithUTF8CString("size");
  JSObjectSetProperty(ctx, obj, k, JSValueMakeNumber(ctx, (double)fs->size),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  k = JSStringCreateWithUTF8CString("mode");
  JSObjectSetProperty(ctx, obj, k, JSValueMakeNumber(ctx, (double)fs->mode),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  const char* kind = "other";
  if (fs->kind == FILE_KIND_REG) kind = "reg";
  else if (fs->kind == FILE_KIND_DIR) kind = "dir";
  else if (fs->kind == FILE_KIND_LNK) kind = "lnk";
  k = JSStringCreateWithUTF8CString("kind");
  JSObjectSetProperty(ctx, obj, k, JSOfCString(kind), kJSPropertyAttributeNone,
                      NULL);
  JSStringRelease(k);
  k = JSStringCreateWithUTF8CString("mtime");
  JSObjectSetProperty(ctx, obj, k,
                      JSBigIntCreateWithUInt64(ctx, (uint64_t)fs->mtime, NULL),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  k = JSStringCreateWithUTF8CString("atime");
  JSObjectSetProperty(ctx, obj, k,
                      JSBigIntCreateWithUInt64(ctx, (uint64_t)fs->atime, NULL),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  return obj;
}

//  io.stat(path) -> {size, mode, kind, mtime, atime}  (follows symlinks)
static JABC_FN(JABCioStat) {
  if (argc < 1) JABC_THROW("io.stat(path)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.stat(): bad path");
  }
  filestat fs = {};
  ok64 o = FILEStat(&fs, $path(path));
  if (o != OK) JABC_THROW(strerror(errno));
  return JABCStatObj(ctx, &fs);
}

//  io.lstat(path) -> {size, mode, kind, mtime, atime}  (does NOT follow the
//  symlink: a link reports kind "lnk", and a dangling link stats fine — lstat
//  inspects the link itself, never its target).
static JABC_FN(JABCioLStat) {
  if (argc < 1) JABC_THROW("io.lstat(path)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.lstat(): bad path");
  }
  filestat fs = {};
  ok64 o = FILELStat(&fs, $path(path));
  if (o != OK) JABC_THROW(strerror(errno));
  return JABCStatObj(ctx, &fs);
}

//  io.readlink(path) -> string  (the symlink's target, over FILEReadLink).
//  Reads into a stack path buffer; the target bytes cross as a JS string (no
//  NUL appended by the leaf, so length is exact).  Pure marshalling.
static JABC_FN(JABCioReadLink) {
  if (argc < 1) JABC_THROW("io.readlink(path) -> string");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.readlink(): bad path");
  }
  a_path(target);
  if (FILEReadLink(target, $path(path)) != OK) JABC_THROW(strerror(errno));
  return JABCStrOfSlice(ctx, u8bDataC(target), exception);
}

//  io.symlink(target, linkpath) -> create a symlink `linkpath` pointing at
//  `target` (over FILESymLink).  `target` is stored verbatim (may be relative
//  / dangling).  Pure marshalling; throws if linkpath already exists.
static JABC_FN(JABCioSymLink) {
  if (argc < 2) JABC_THROW("io.symlink(target, linkpath)");
  a_pad(u8, target, FILE_PATH_MAX_LEN);
  a_pad(u8, linkpath, FILE_PATH_MAX_LEN);
  if (JABCPath(target, ctx, args[0], exception) != OK ||
      JABCPath(linkpath, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.symlink(): bad path");
  }
  if (FILESymLink($path(target), $path(linkpath)) != OK)
    JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.chmod(path, mode) -> set the POSIX permission bits (over FILEChmod).
//  `mode` is the usual octal int (e.g. 0o755); pure marshalling.
static JABC_FN(JABCioChmod) {
  if (argc < 2) JABC_THROW("io.chmod(path, mode)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.chmod(): bad path");
  }
  double dm = JSValueToNumber(ctx, args[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (dm < 0) JABC_THROW("io.chmod(): negative mode");
  if (FILEChmod($path(path), (u32)dm) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.setMtime(path, ron60) -> stamp a file's atime+mtime to the ron60 instant
//  (over FILESetMtime / utimensat NOFOLLOW: a symlink stamps the link itself).
//  ron60 crosses as a BigInt, like io.stat's mtime and ron.encode (JS-047).
static JABC_FN(JABCioSetMtime) {
  if (argc < 2) JABC_THROW("io.setMtime(path, ron60)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.setMtime(): bad path");
  }
  uint64_t ts = JSValueToUInt64(ctx, args[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (FILESetMtime($path(path), (ron60)ts) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.readdir — one entry point over FILEScanDir / FILEDeepScanDir with a
//  POLYMORPHIC 2nd arg.  FILEScanDir delivers each entry as the FULL iterator
//  path (the scanned root + the entry name; '.'/'..' already skipped, dir
//  entries carry a trailing '/').  Every shape emits the path RELATIVE to the
//  scanned root, KEEPING the dir's trailing '/' as the file-vs-dir marker (e.g.
//  "sub/", "sub/child").  At depth 0 the relative path is just the basename, so
//  the one-level form lists "alpha", "sub/".
//
//  The 2nd arg is dispatched in the native frame:
//   - absent              -> form 1, options all default.
//   - a function          -> sugar for {callback: fn} (the directed cb).
//   - an options object    -> {recursive, callback, hidden} (any subset).
//   - anything else        -> a TypeError-style throw.
//  Resulting behaviors:
//   1. io.readdir(path[, {recursive}]) -> string[]   array form (no callback):
//        one level, or — with recursive:true — the flat full subtree via the
//        native FILE_SCAN_DEEP (FILEDeepScanDir).  Dirs marked.
//   2. io.readdir(path, fn|{callback:fn[, recursive]}) -> undefined   cb(name)
//        per entry; the cb return is a directive: "more"/truthy/undefined =
//        continue, "enough"/false = stop the WHOLE scan, "recur" = descend into
//        this dir first (meaningful only when recursive is false; a no-op once
//        recursive:true already descends the whole tree).  The cb runs
//        synchronously inside the scan frame and is never stashed (rule #4); a
//        cb throw aborts the scan and propagates.
//   hidden (default false): basenames starting '.' are SKIPPED, and hidden dirs
//        are NOT descended (the skip returns FILESKIP, which both omits the
//        entry and prunes recursion — for the native deep scan and the per-entry
//        "recur" alike).  hidden:true includes dotfiles and descends hidden dirs.
//
//  Aborts (cb "enough" / a cb throw) leave the scan via a private non-OK code
//  so they unwind FILEScanRecurse without masquerading as a FILE error; the
//  caller tells an abort from a real error by inspecting the context (`stop` /
//  `*exception`), never by the code's identity.
con ok64 JABCSCANSTOP = 0x4a414253544f50;  //  "JABSTOP" — binding-private, never RON-decoded

//  Build the root-relative, dir-marked entry name from the full iterator path
//  into the caller's scratch path buffer.  Empty (the root itself) -> NODATA.
static ok64 JABCReaddirRel(path8b out, path8p full, size_t rootlen) {
  a_dup(u8c, data, u8bDataC(full));
  if ($len(data) <= (ssize_t)(rootlen + 1)) return NODATA;
  //  Skip "<root>/" — a_rest does the offset arithmetic with bounds checks.
  a_rest(u8c, rel, data, rootlen + 1);
  b8 slash = (!$empty(rel) && *$last(rel) == '/');
  if (slash) u8csShed1(rel);  //  PATHu8bPush rejects a trailing '/'
  if ($empty(rel)) return NODATA;
  ok64 o = PATHu8bDup(out, rel);
  if (o != OK) return o;
  if (slash) {  //  re-mark the dir
    a_cstr(s, "/");
    o = PATHu8bFeed(out, s);
    if (o != OK) return o;
  }
  return OK;
}

//  Is this entry hidden? — its basename starts with '.'.  Operates on the FULL
//  iterator path: alias its DATA, drop a trailing '/' (dir entries carry one),
//  take the basename, test the first byte.  '.'/'..' are already dropped by the
//  scan, so any leading-dot basename here is a real dotfile / dot-dir.
static b8 JABCReaddirIsHidden(path8p full) {
  a_dup(u8c, data, u8bDataC(full));
  if (!$empty(data) && *$last(data) == '/') u8csShed1(data);
  u8cs base = {};
  PATHu8sBase(base, data);
  return !$empty(base) && $at(base, 0) == '.';
}

struct JABCReaddirCtx {
  JSContextRef ctx;
  JSObjectRef arr;        //  array-building forms (no cb); NULL for the cb form
  JSObjectRef cb;         //  cb form; NULL otherwise
  size_t rootlen;         //  byte length of the scanned root (relative-path base)
  unsigned n;             //  next array index
  b8 recursive;           //  native deep scan in flight ("recur" is a no-op)
  b8 hidden;              //  include dotfiles + descend hidden dirs
  b8 stop;                //  cb said "enough" — abort, but not an error
  JSValueRef* exception;  //  cb threw — abort and propagate
};

//  Emit one entry into the result array (no-callback array form).
static ok64 JABCReaddirEmit(void0p arg, path8p path) {
  JABCReaddirCtx* c = (JABCReaddirCtx*)arg;
  //  Hidden filter: FILESKIP both omits this entry AND prunes the deep scan
  //  from descending into it (a hidden dir).
  if (!c->hidden && JABCReaddirIsHidden(path)) return FILESKIP;
  a_path(rel);
  ok64 o = JABCReaddirRel(rel, path, c->rootlen);
  if (o == NODATA) return OK;
  if (o != OK) return o;
  JSStringRef s = JSStringCreateWithUTF8CString((const char*)$path(rel)[0]);
  JSObjectSetPropertyAtIndex(c->ctx, c->arr, c->n++,
                             JSValueMakeString(c->ctx, s), NULL);
  JSStringRelease(s);
  return OK;
}

//  Map a cb's return value onto a scan directive.  "recur" is signalled to the
//  trampoline via *recur (caller descends); the ok64 return drives continue/stop.
static ok64 JABCReaddirDirective(JABCReaddirCtx* c, JSValueRef r, b8* recur) {
  *recur = NO;
  if (JSValueIsString(c->ctx, r)) {
    char tag[8] = "";
    JSStringRef s = JSValueToStringCopy(c->ctx, r, NULL);
    JSStringGetUTF8CString(s, tag, sizeof(tag));
    JSStringRelease(s);
    if (strcmp(tag, "enough") == 0) { c->stop = YES; return JABCSCANSTOP; }
    if (strcmp(tag, "recur") == 0) { *recur = YES; return OK; }
    return OK;  //  "more" and any other string -> continue
  }
  //  Non-string: false -> stop, truthy/undefined -> continue.
  if (!JSValueIsUndefined(c->ctx, r) && !JSValueToBoolean(c->ctx, r)) {
    c->stop = YES;
    return JABCSCANSTOP;
  }
  return OK;
}

//  cb-form trampoline: call cb(name), map the directive, descend on "recur".
static ok64 JABCReaddirCall(void0p arg, path8p path) {
  JABCReaddirCtx* c = (JABCReaddirCtx*)arg;
  //  Hidden filter: FILESKIP omits this entry AND, under a native deep scan
  //  (recursive:true), prunes descent into a hidden dir.  In the per-entry
  //  (recursive:false) form the cb never gets a chance to "recur" into it.
  if (!c->hidden && JABCReaddirIsHidden(path)) return FILESKIP;
  a_path(rel);
  ok64 o = JABCReaddirRel(rel, path, c->rootlen);
  if (o == NODATA) return OK;
  if (o != OK) return o;
  JSStringRef ns = JSStringCreateWithUTF8CString((const char*)$path(rel)[0]);
  JSValueRef name = JSValueMakeString(c->ctx, ns);
  JSStringRelease(ns);
  JSValueRef exc = NULL;
  JSValueRef r = JSObjectCallAsFunction(c->ctx, c->cb, NULL, 1, &name, &exc);
  if (exc != NULL) {  //  a cb throw aborts the scan and propagates
    *c->exception = exc;
    return JABCSCANSTOP;
  }
  b8 recur = NO;
  o = JABCReaddirDirective(c, r, &recur);
  if (o != OK) return o;  //  "enough"/false
  //  "recur" is a no-op once a native deep scan already descends every dir.
  if (recur && !c->recursive) {
    //  Descend via a NESTED FILEScanDir on the subdir.  The cb form decides
    //  recursion per entry, so FILE_SCAN_DEEP (which descends unconditionally)
    //  can't express it — we hand-roll one nested scan on a FRESH path buffer
    //  (the live iterator buffer must not be re-driven mid-iteration).  rootlen
    //  stays the ORIGINAL root, so nested entries come out relative ("sub/child").
    a_path(sub);
    ok64 d = PATHu8bDup(sub, u8bDataC(path));
    if (d != OK) return d;
    d = FILEScanDir(sub, JABCReaddirCall, c);
    if (d != OK && d != JABCSCANSTOP) return d;
    if (d == JABCSCANSTOP) return d;  //  propagate stop/throw out of the tree
  }
  return OK;
}

//  Read a boolean property off the options object (absent -> false).
static b8 JABCReaddirOptBool(JSContextRef ctx, JSObjectRef opts, const char* key) {
  JSStringRef k = JSStringCreateWithUTF8CString(key);
  JSValueRef v = JSObjectGetProperty(ctx, opts, k, NULL);
  JSStringRelease(k);
  return JSValueToBoolean(ctx, v);
}

static JABC_FN(JABCioReaddir) {
  if (argc < 1) JABC_THROW("io.readdir(path[, fn|{recursive,callback,hidden}])");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.readdir(): bad path");
  }
  size_t rootlen = (size_t)u8bDataLen(path);

  //  Dispatch the polymorphic 2nd arg into {cb, recursive, hidden}.
  JSObjectRef cb = NULL;
  b8 recursive = NO, hidden = NO;
  if (argc > 1 && !JSValueIsUndefined(ctx, args[1]) &&
      !JSValueIsNull(ctx, args[1])) {
    if (!JSValueIsObject(ctx, args[1]))
      JABC_THROW("io.readdir(): 2nd arg must be a function or an options object");
    JSObjectRef o2 = (JSObjectRef)args[1];
    if (JSObjectIsFunction(ctx, o2)) {
      cb = o2;  //  bare function is sugar for {callback: fn}
    } else {
      recursive = JABCReaddirOptBool(ctx, o2, "recursive");
      hidden = JABCReaddirOptBool(ctx, o2, "hidden");
      JSStringRef k = JSStringCreateWithUTF8CString("callback");
      JSValueRef cbv = JSObjectGetProperty(ctx, o2, k, NULL);
      JSStringRelease(k);
      if (!JSValueIsUndefined(ctx, cbv) && JSValueIsObject(ctx, cbv) &&
          JSObjectIsFunction(ctx, (JSObjectRef)cbv))
        cb = (JSObjectRef)cbv;
    }
  }

  //  Callback present -> directed scan, returns undefined.  recursive picks the
  //  native deep scan (FILEDeepScanDir, descends every dir) vs the one-level
  //  scan where the cb decides per-entry "recur" nesting.
  if (cb != NULL) {
    JABCReaddirCtx c = {ctx, NULL, cb, rootlen, 0, recursive, hidden, NO,
                        exception};
    ok64 o = recursive ? FILEDeepScanDir(path, JABCReaddirCall, &c)
                       : FILEScanDir(path, JABCReaddirCall, &c);
    if (*exception) return JSValueMakeUndefined(ctx);  //  cb threw
    if (o != OK && o != JABCSCANSTOP) JABC_THROW(strerror(errno));
    JABC_UNDEF;
  }

  //  No callback -> build the flat result array in this binding frame; recursive
  //  uses the native deep scan, otherwise one level.  Dirs stay marked.
  JSObjectRef arr = JSObjectMakeArray(ctx, 0, NULL, exception);
  if (*exception || arr == NULL) return JSValueMakeUndefined(ctx);
  JABCReaddirCtx c = {ctx, arr, NULL, rootlen, 0, recursive, hidden, NO,
                      exception};
  ok64 o = recursive ? FILEDeepScanDir(path, JABCReaddirEmit, &c)
                     : FILEScanDir(path, JABCReaddirEmit, &c);
  if (o != OK) JABC_THROW(strerror(errno));
  return arr;
}

//  io._read(fd, u8) -> n  (0 = EOF).  Single read into the typed array.
static JABC_FN(JABCioRead) {
  if (argc < 2) JABC_THROW("io._read(fd, Uint8Array)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  u8s ta = {};
  if (!JABCBytesOf(ta, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  ssize_t n;
  do { n = read(fd, ta[0], $len(ta)); } while (n < 0 && errno == EINTR);
  if (n < 0) JABC_THROW(strerror(errno));
  return JSValueMakeNumber(ctx, (double)n);
}

//  io._write(fd, u8) -> n.  Single write of the typed array's bytes.
static JABC_FN(JABCioWrite) {
  if (argc < 2) JABC_THROW("io._write(fd, Uint8Array)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  u8s ta = {};
  if (!JABCBytesOf(ta, ctx, args[1], exception)) return JSValueMakeUndefined(ctx);
  ssize_t n;
  do { n = write(fd, ta[0], $len(ta)); } while (n < 0 && errno == EINTR);
  if (n < 0) JABC_THROW(strerror(errno));
  return JSValueMakeNumber(ctx, (double)n);
}

//  Mapped-file deallocator: JS GC drives the munmap (buf is the FILE_WANT_BUFS
//  slot handed to FILEUnMap).
static void JABCMapFree(void* bytes, void* ctx) {
  (void)bytes;
  FILEUnMap((u8bp)ctx);
}

//  io._mmap(path, "r"|"rw"|"c", size) -> Uint8Array  (munmap on GC)
static JABC_FN(JABCioMmap) {
  if (argc < 1) JABC_THROW("io._mmap(path, mode, size)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io._mmap(): bad path");
  }
  char mode[8] = "r";
  if (argc > 1 && JSValueIsString(ctx, args[1])) {
    JSStringRef s = JSValueToStringCopy(ctx, args[1], NULL);
    JSStringGetUTF8CString(s, mode, sizeof(mode));
    JSStringRelease(s);
  }
  u8bp buf = NULL;
  ok64 o;
  if (strcmp(mode, "c") == 0) {
    double sz = argc > 2 ? JSValueToNumber(ctx, args[2], NULL) : 0;
    if (sz < 0) JABC_THROW("io._mmap(): negative size");
    o = FILEMapCreate(&buf, $path(path), (size_t)sz);
  } else if (strcmp(mode, "rw") == 0) {
    o = FILEMapRW(&buf, $path(path));
  } else {
    o = FILEMapRO(&buf, $path(path));
  }
  if (o != OK || buf == NULL) JABC_THROW(strerror(errno));
  //  Expose the WHOLE mapping (DATA + IDLE): a created file has DATA empty and
  //  everything in IDLE, a mapped existing file has it all in DATA — either
  //  way the container owns the full region.
  size_t mlen = u8bDataLen(buf) + u8bIdleLen(buf);
  JSValueRef ta = JSObjectMakeTypedArrayWithBytesNoCopy(
      ctx, kJSTypedArrayTypeUint8Array, u8bData(buf)[0], mlen,
      JABCMapFree, (void*)buf, exception);
  if (*exception || ta == NULL) {
    FILEUnMap(buf);
    return JSValueMakeUndefined(ctx);
  }
  return ta;
}

//  Anonymous-mapping deallocator: munmap the whole region, free the length box.
static void JABCRamFree(void* bytes, void* ctx) {
  size_t* len = (size_t*)ctx;
  munmap(bytes, *len);
  free(len);
}

//  io._ram(n) -> Uint8Array  (anonymous mmap, faults in lazily, munmap on GC)
static JABC_FN(JABCioRam) {
  if (argc < 1) JABC_THROW("io._ram(n)");
  double dn = JSValueToNumber(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (dn <= 0) JABC_THROW("io._ram(): size must be > 0");
  size_t n = (size_t)dn;
  void* map = mmap(NULL, n, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED) JABC_THROW(strerror(errno));
  size_t* len = (size_t*)malloc(sizeof(size_t));
  if (!len) {
    munmap(map, n);
    JABC_THROW(strerror(errno));
  }
  *len = n;
  JSValueRef ta = JSObjectMakeTypedArrayWithBytesNoCopy(
      ctx, kJSTypedArrayTypeUint8Array, map, n, JABCRamFree, len, exception);
  if (*exception || ta == NULL) {
    munmap(map, n);
    free(len);
    return JSValueMakeUndefined(ctx);
  }
  return ta;
}

//  io._msync(u8) -> flush the typed array's pages (works on the raw range,
//  no buffer-descriptor lookup needed).
static JABC_FN(JABCioMsync) {
  if (argc < 1) JABC_THROW("io._msync(Uint8Array)");
  u8s ta = {};
  if (!JABCBytesOf(ta, ctx, args[0], exception)) return JSValueMakeUndefined(ctx);
  if ($len(ta) && msync(ta[0], $len(ta), MS_SYNC) != 0)
    JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io._truncate(path, bytes) -> resize a file on disk (trim a booked/over-
//  allocated output to its live size).  Reopens by path + ftruncate so no
//  persistent fd/descriptor is held — reconstructed per call.
static JABC_FN(JABCioTruncate) {
  if (argc < 2) JABC_THROW("io._truncate(path, bytes)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io._truncate(): bad path");
  }
  double db = JSValueToNumber(ctx, args[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  if (db < 0) JABC_THROW("io._truncate(): negative size");
  int fd = -1;
  if (FILEOpen(&fd, $path(path), O_RDWR) != OK || fd < 0)
    JABC_THROW(strerror(errno));
  ok64 o = FILEResize(&fd, (size_t)db);
  FILEClose(&fd);
  if (o != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.cwd() -> string  (the process working directory, via getcwd(3)).
static JABC_FN(JABCioCwd) {
  (void)args;
  (void)argc;
  a_path(cwd);
  if (FILEGetCwd(cwd) != OK) JABC_THROW(strerror(errno));
  return JABCStrOfSlice(ctx, u8bDataC(cwd), exception);
}

//  io.chdir(path) -> set the process working directory (over chdir(2)).  The
//  cwd getter's setter: mirrors io.mkdir's validation + errno propagation, so
//  on success io.cwd() reflects the new dir and on failure (ENOENT/ENOTDIR/
//  EACCES) it throws strerror(errno) with the cwd unchanged.  The pager
//  (BRO-024) chdir's into a repo before running verbs.
static JABC_FN(JABCioChdir) {
  if (argc < 1) JABC_THROW("io.chdir(path)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.chdir(): bad path");
  }
  if (chdir((char const*)*$path(path)) != 0) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.getenv(name) -> string | undefined.  FILEGetEnv yields an empty slice
//  for an unset (or empty-valued) var; either way we return `undefined`.
static JABC_FN(JABCioGetenv) {
  if (argc < 1 || !JSValueIsString(ctx, args[0]))
    JABC_THROW("io.getenv(name) -> string|undefined");
  char name[PAGESIZE];
  JSStringRef s = JSValueToStringCopy(ctx, args[0], exception);
  if (*exception || s == NULL) return JSValueMakeUndefined(ctx);
  JSStringGetUTF8CString(s, name, sizeof(name));
  JSStringRelease(s);
  u8cs val = {};
  FILEGetEnv(name, val);
  if (u8csEmpty(val)) return JSValueMakeUndefined(ctx);
  //  JS-108: length-exact — a >PAGESIZE env value is no longer clamped.
  return JABCStrOfSlice(ctx, val, exception);
}

//  io.getpid() -> number  (this process's PID, via getpid(2)).  JOBQ keys the
//  per-process /tmp queue name on it (core/loop.js _pid) so a dead run's queue
//  is never resumed and two concurrent runs never collide.  No /proc, portable.
static JABC_FN(JABCioGetpid) {
  (void)args;
  (void)argc;
  return JSValueMakeNumber(ctx, (double)getpid());
}

//  io.unlink(path) -> remove a name from the filesystem (over FILEUnLink).  Pure
//  marshalling.  (A file mmap'd before unlink stays valid: the inode lives on
//  while mapped, so the page-cache Buf auto-cleans when GC'd — see API.md.)
static JABC_FN(JABCioUnlink) {
  if (argc < 1) JABC_THROW("io.unlink(path)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.unlink(): bad path");
  }
  if (FILEUnLink($path(path)) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.rename(old, new) -> atomic rename within a filesystem (over FILERename).
//  Pure marshalling.  The LSM flush path (JS-022) books a run under a temp name,
//  closes it, then renames it into the final run file so readers never see a
//  half-written run.
static JABC_FN(JABCioRename) {
  if (argc < 2) JABC_THROW("io.rename(old, new)");
  a_pad(u8, oldp, FILE_PATH_MAX_LEN);
  a_pad(u8, newp, FILE_PATH_MAX_LEN);
  if (JABCPath(oldp, ctx, args[0], exception) != OK ||
      JABCPath(newp, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.rename(): bad path");
  }
  if (FILERename($path(oldp), $path(newp)) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.mkdir(path) -> create a directory (and parents) over FILEMakeDirP.  Pure
//  marshalling; idempotent (an existing dir is fine).  The index (JS-022) calls
//  it to materialise its run directory before booking the first run.
static JABC_FN(JABCioMkdir) {
  if (argc < 1) JABC_THROW("io.mkdir(path)");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.mkdir(): bad path");
  }
  if (FILEMakeDirP($path(path)) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  io.rmdir(path[, recursive]) -> remove a directory (over FILERmDir).  Plain
//  rmdir by default (empty dir only; ENOTEMPTY otherwise); recursive:true does
//  an rm -rf of the whole subtree.  GET-039: checkout calls it (recursive) to
//  replace a tracked DIR with a leaf on a type-change (dir->file / dir->link),
//  which io.unlink cannot do (unlink throws EISDIR on any directory).
static JABC_FN(JABCioRmdir) {
  if (argc < 1) JABC_THROW("io.rmdir(path[, recursive])");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.rmdir(): bad path");
  }
  bool recursive = argc > 1 ? JSValueToBoolean(ctx, args[1]) : false;
  if (FILERmDir($path(path), recursive) != OK) JABC_THROW(strerror(errno));
  JABC_UNDEF;
}

//  . . . . . . . . process spawn + reap (JS-020) . . . . . . . .
//
//  argv is a JS string[] -> a u8css (slice of u8cs) over per-call STACK
//  scratch: each element's UTF-8 bytes land NUL-terminated in `bytes`, and the
//  matching {head, term} pair (term BEFORE the NUL) goes into `slots`.  Mirrors
//  the WIRECLI.c / dog/HOME.c argv idiom (a `u8cs argv_arr[]` + `u8css argv =
//  {arr, arr+n}`).  FILESpawn re-copies argv internally for execv, so the
//  scratch only has to live across the spawn call.  No held JS ref (rule #4);
//  fds + pid cross the boundary as plain numbers.
#define JABC_ARGV_MAX 256       //  argv element cap (stack slot array)
#define JABC_ARGV_BYTES 65536   //  total argv UTF-8 byte cap (stack scratch)

//  Fill `slots`/`bytes`/`*n` from a JS array argument; returns OK or a code.
//  `slots` is a u8cs[JABC_ARGV_MAX]; `bytes` a u8b over JABC_ARGV_BYTES scratch.
static ok64 JABCBuildArgv(u8cs* slots, size_t* n, path8b bytes, JSContextRef ctx,
                          JSValueRef arg, JSValueRef* exception) {
  if (!JSValueIsArray(ctx, arg)) return BADARG;
  JSObjectRef arr = (JSObjectRef)arg;
  JSStringRef lk = JSStringCreateWithUTF8CString("length");
  double len = JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lk, NULL), NULL);
  JSStringRelease(lk);
  if (len < 1) return BADARG;  //  argv[0] (program name) is mandatory
  if (len > JABC_ARGV_MAX) return NOROOM;
  size_t cnt = (size_t)len;
  for (size_t i = 0; i < cnt; i++) {
    JSValueRef ev = JSObjectGetPropertyAtIndex(ctx, arr, (unsigned)i, exception);
    if (*exception || !JSValueIsString(ctx, ev)) return BADARG;
    JSStringRef s = JSValueToStringCopy(ctx, ev, exception);
    if (*exception || s == NULL) return BADARG;
    //  Copy into the shared scratch buffer's IDLE; record the slice (sans NUL).
    u8* head = u8bIdleHead(bytes);
    size_t room = (size_t)u8bIdleLen(bytes);
    size_t got = JSStringGetUTF8CString(s, (char*)head, room);  //  got counts NUL
    JSStringRelease(s);
    if (got < 1 || got >= room) return NOROOM;
    u8cs slot = {head, head + (got - 1)};
    u8csMv(slots[i], slot);
    //  Park the copied bytes + NUL in PAST so the next element gets fresh IDLE.
    u8bFed(bytes, got);
    u8bUsed(bytes, got);
  }
  *n = cnt;
  return OK;
}

//  Set a numeric property on an object (small helper for the result records).
static void JABCSetNum(JSContextRef ctx, JSObjectRef o, const char* k, double v) {
  JSStringRef ks = JSStringCreateWithUTF8CString(k);
  JSObjectSetProperty(ctx, o, ks, JSValueMakeNumber(ctx, v),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(ks);
}

//  io.spawn(path, argv) -> {pid, stdin, stdout}.  Creates pipes for the child's
//  stdin (write fd) and stdout (read fd); stderr is INHERITED from the parent.
//  The caller owns + closes both fds and reaps the pid via io.reap.
static JABC_FN(JABCioSpawn) {
  if (argc < 2) JABC_THROW("io.spawn(path, argv) -> {pid, stdin, stdout}");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.spawn(): bad path");
  }
  a_pad(u8, bytes, JABC_ARGV_BYTES);
  u8cs slots[JABC_ARGV_MAX];
  size_t nargs = 0;
  if (JABCBuildArgv(slots, &nargs, bytes, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.spawn(): argv must be a non-empty string[]");
  }
  u8css argv = {slots, slots + nargs};
  u8csc binpath = {$path(path)[0], $path(path)[1]};
  int wfd = -1, rfd = -1;
  pid_t pid = 0;
  if (FILESpawn(binpath, argv, &wfd, &rfd, &pid) != OK)
    JABC_THROW(strerror(errno));
  JSObjectRef obj = JSObjectMake(ctx, NULL, NULL);
  JABCSetNum(ctx, obj, "pid", (double)pid);
  JABCSetNum(ctx, obj, "stdin", (double)wfd);
  JABCSetNum(ctx, obj, "stdout", (double)rfd);
  return obj;
}

//  io.spawnFds(path, argv, inFd, outFd) -> pid.  The child dups the supplied
//  fds (`-1` inherits the parent's); the caller still owns + closes them.
static JABC_FN(JABCioSpawnFds) {
  if (argc < 2) JABC_THROW("io.spawnFds(path, argv, inFd, outFd) -> pid");
  a_pad(u8, path, FILE_PATH_MAX_LEN);
  if (JABCPath(path, ctx, args[0], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.spawnFds(): bad path");
  }
  a_pad(u8, bytes, JABC_ARGV_BYTES);
  u8cs slots[JABC_ARGV_MAX];
  size_t nargs = 0;
  if (JABCBuildArgv(slots, &nargs, bytes, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("io.spawnFds(): argv must be a non-empty string[]");
  }
  u8css argv = {slots, slots + nargs};
  int inFd = argc > 2 ? JABCInt(ctx, args[2], exception) : -1;
  int outFd = argc > 3 ? JABCInt(ctx, args[3], exception) : -1;
  if (*exception) return JSValueMakeUndefined(ctx);
  u8csc binpath = {$path(path)[0], $path(path)[1]};
  pid_t pid = 0;
  if (FILESpawnFds(binpath, argv, inFd, outFd, &pid) != OK)
    JABC_THROW(strerror(errno));
  return JSValueMakeNumber(ctx, (double)pid);
}

//  io.reap(pid) -> {code} on a clean exit (any status), {signal} on a signal
//  death (FILESIGNAL).  Exactly one key is set.  Any other non-OK throws.
static JABC_FN(JABCioReap) {
  if (argc < 1) JABC_THROW("io.reap(pid) -> {code}|{signal}");
  int pid = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  int rc = -1;
  ok64 o = FILEReap((pid_t)pid, &rc);
  if (o != OK && o != FILESIGNAL) JABC_THROW(strerror(errno));
  JSObjectRef obj = JSObjectMake(ctx, NULL, NULL);
  JABCSetNum(ctx, obj, o == FILESIGNAL ? "signal" : "code", (double)rc);
  return obj;
}

//  io.isatty(fd) -> bool  (is the fd a terminal? — the color-vs-plain gate)
static JABC_FN(JABCioIsatty) {
  if (argc < 1) JABC_THROW("io.isatty(fd)");
  int fd = JABCInt(ctx, args[0], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  return JSValueMakeBoolean(ctx, isatty(fd) == 1);
}

//  io.log(...args) -> write strings / typed arrays to stderr + newline.
static JABC_FN(JABCioLog) {
  for (size_t i = 0; i < argc; i++) {
    if (JSValueIsString(ctx, args[i])) {
      JSStringRef s = JSValueToStringCopy(ctx, args[i], exception);
      if (*exception) return JSValueMakeUndefined(ctx);
      size_t max = JSStringGetMaximumUTF8CStringSize(s);
      char* b = (char*)malloc(max);
      size_t got = JSStringGetUTF8CString(s, b, max);
      u8s span = {(u8*)b, (u8*)b + (got ? got - 1 : 0)};
      while ($len(span) > 0) {
        ssize_t w = write(STDERR_FILENO, span[0], $len(span));
        if (w <= 0) break;
        span[0] += w;
      }
      free(b);
      JSStringRelease(s);
    } else if (JSValueGetTypedArrayType(ctx, args[i], NULL) !=
               kJSTypedArrayTypeNone) {
      u8s ta = {};
      if (!JABCBytesOf(ta, ctx, args[i], exception))
        return JSValueMakeUndefined(ctx);
      ssize_t w = write(STDERR_FILENO, ta[0], $len(ta));
      (void)w;
    }
  }
  ssize_t w = write(STDERR_FILENO, "\n", 1);
  (void)w;
  JABC_UNDEF;
}

ok64 JABCioInstall() {
  FILEInit();
  JABC_API_OBJECT(io);
  JABC_API_FN(io, "open", JABCioOpen);
  JABC_API_FN(io, "close", JABCioClose);
  JABC_API_FN(io, "sync", JABCioSync);
  JABC_API_FN(io, "size", JABCioSize);
  JABC_API_FN(io, "resize", JABCioResize);
  JABC_API_FN(io, "lock", JABCioLock);
  JABC_API_FN(io, "unlock", JABCioUnlock);
  JABC_API_FN(io, "stat", JABCioStat);
  JABC_API_FN(io, "lstat", JABCioLStat);
  JABC_API_FN(io, "readlink", JABCioReadLink);
  JABC_API_FN(io, "symlink", JABCioSymLink);
  JABC_API_FN(io, "chmod", JABCioChmod);
  JABC_API_FN(io, "setMtime", JABCioSetMtime);
  JABC_API_FN(io, "readdir", JABCioReaddir);
  JABC_API_FN(io, "_read", JABCioRead);
  JABC_API_FN(io, "_write", JABCioWrite);
  JABC_API_FN(io, "_mmap", JABCioMmap);
  JABC_API_FN(io, "_ram", JABCioRam);
  JABC_API_FN(io, "_msync", JABCioMsync);
  JABC_API_FN(io, "_truncate", JABCioTruncate);
  JABC_API_FN(io, "unlink", JABCioUnlink);
  JABC_API_FN(io, "rename", JABCioRename);
  JABC_API_FN(io, "mkdir", JABCioMkdir);
  JABC_API_FN(io, "rmdir", JABCioRmdir);
  JABC_API_FN(io, "spawn", JABCioSpawn);
  JABC_API_FN(io, "spawnFds", JABCioSpawnFds);
  JABC_API_FN(io, "reap", JABCioReap);
  JABC_API_FN(io, "isatty", JABCioIsatty);
  JABC_API_FN(io, "cwd", JABCioCwd);
  JABC_API_FN(io, "chdir", JABCioChdir);
  JABC_API_FN(io, "getpid", JABCioGetpid);
  JABC_API_FN(io, "getenv", JABCioGetenv);
  JABC_API_FN(io, "log", JABCioLog);
  return OK;
}

ok64 JABCioUninstall() {
  FILECloseAll();
  return OK;
}
