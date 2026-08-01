#include "JABC.hpp"
extern "C" {
#include "abc/FSW.h"
}

//  JAB-031: fsw binds abc/FSW — inotify (Linux) / kqueue (macOS/BSD).  Rule #4
//  holds: C keeps NO per-watch state; only the fd and the caller's Buf cross.
//  drain packs (wd, len, name) records; the map + pol wiring are in the bundle
//  below.  FSWPoll gets no binding — pol.run owns the blocking.  See API.md.

//  The drain sink: a caller-owned Buf plus the record count.  Lives on the
//  leaf's stack, dies with the call — no memory, no JS refs.
struct JABCFswSink {
  u8bp out;
  u32 n;
};

//  JAB-031: one packed record — u32 wd, u32 name length (both LE), name bytes.
//  Room is checked whole, so IDLE never holds a torn record.
static ok64 JABCFswPack(u8cs name, void* ctx) {
  JABCFswSink* s = (JABCFswSink*)ctx;
  u32 len = (u32)u8csLen(name);
  u32 wd = 0;  //  FSWDrain reports no watch descriptor; slot reserved
  if (u8bIdleLen(s->out) < sizeof(wd) + sizeof(len) + (size_t)len)
    return FSWNOROOM;
  u8sFeed32(u8bIdle(s->out), &wd);
  u8sFeed32(u8bIdle(s->out), &len);
  u8csc bytes = {name[0], name[1]};
  ok64 o = u8bFeed(s->out, bytes);
  if (o != OK) return o;
  s->n++;
  return OK;
}

//  fsw.init() -> wfd (a pollable fd; hand it to pol.watch(wfd, pol.IN, ...))
static JABC_FN(JABCFswInit) {
  (void)args;
  (void)argc;
  int wfd = -1;
  if (FSWInit(&wfd) != OK) JABC_THROW("fsw.init: cannot create a watcher");
  return JSValueMakeNumber(ctx, (double)wfd);
}

//  fsw.dir(wfd, path) -> wd.  Non-recursive, one dir level.
static JABC_FN(JABCFswDir) {
  if (argc < 2) JABC_THROW("fsw.dir(wfd, path)");
  u64 wfd = 0;
  if (!JABCu64Of(&wfd, ctx, args[0], exception)) JABC_UNDEF;
  a_pad(u8, p, FILE_PATH_MAX_LEN);
  if (JABCPath(p, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("fsw.dir: bad path");
  }
  if (FSWDir((int)wfd, $path(p)) != OK)
    JABC_THROW("fsw.dir: cannot watch that directory");
  return JSValueMakeNumber(ctx, 0);  //  see JABCFswPack: no wd is exposed yet
}

//  fsw.drain(wfd, buf) -> record count.  Non-blocking; 0 means nothing queued.
static JABC_FN(JABCFswDrain) {
  if (argc < 2) JABC_THROW("fsw.drain(wfd, buf)");
  u64 wfd = 0;
  if (!JABCu64Of(&wfd, ctx, args[0], exception)) JABC_UNDEF;
  if (!JSValueIsObject(ctx, args[1])) JABC_THROW("fsw.drain: buf must be a Buf");
  JSObjectRef bo = JSValueToObject(ctx, args[1], exception);
  if (*exception) return JSValueMakeUndefined(ctx);
  u8* buf[4] = {};  //  cursors gated + checked in arg.cpp
  if (!JABCBufOf(buf, ctx, args[1], exception)) JABC_UNDEF;
  JABCFswSink sink = {buf, 0};
  ok64 o = FSWDrain((int)wfd, JABCFswPack, &sink);
  JABCBufBack(ctx, bo, buf);  //  whole records already packed stay visible
  if (o == FSWNOROOM) JABC_THROW("fsw.drain: the buffer is full, events lost");
  if (o != OK) JABC_THROW("fsw.drain: cannot read the watcher");
  return JSValueMakeNumber(ctx, (double)sink.n);
}

//  fsw.close(wfd) — closes the watcher fd; on kqueue the pinned dir fds stay
//  open (FSW.md), which is why one wfd per watched dir is the JS default.
static JABC_FN(JABCFswClose) {
  if (argc < 1) JABC_THROW("fsw.close(wfd)");
  u64 wfd = 0;
  if (!JABCu64Of(&wfd, ctx, args[0], exception)) JABC_UNDEF;
  FSWClose((int)wfd);
  JABC_UNDEF;
}

//  JAB-031: all per-watch state lives here.  One watcher fd per dir — FSWDrain
//  hands back no wd, so the fd IS the dir's identity until FSW exposes one.
static const char* JABC_FSW_JS = R"JS(
(function (g) {
  "use strict";
  const fsw = g.fsw, io = g.io, pol = g.pol;
  const HDR = 8;                 // u32 wd + u32 name length, little-endian
  const dirs = new Map();        // wfd -> watched dir

  //  parse a drained Buf into [{wd, name}]; name is a BARE basename, and is
  //  "" on kqueue (no filename in the event) — a "rescan this dir" signal.
  fsw.records = (buf) => {
    const b = buf.data ? buf.data() : buf;
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const out = [];
    for (let o = 0; o + HDR <= b.byteLength; ) {
      const wd = dv.getUint32(o, true), len = dv.getUint32(o + 4, true);
      if (o + HDR + len > b.byteLength) break;
      out.push({ wd: wd, name: g.utf8.Decode(b.subarray(o + HDR, o + HDR + len)) });
      o += HDR + len;
    }
    return out;
  };

  //  fsw.watch(dir, fn) -> wfd; fn(name, dir) per event ("" name => rescan).
  fsw.watch = (dir, fn) => {
    const wfd = fsw.init();
    fsw.dir(wfd, dir);
    dirs.set(wfd, dir);
    const buf = io.buf(1 << 16);
    pol.watch(wfd, pol.IN, (fd) => {
      buf.reset();
      fsw.drain(fd, buf);
      const d = dirs.get(fd);
      const rows = fsw.records(buf);
      for (let i = 0; i < rows.length; i++) fn(rows[i].name, d);
      return pol.IN;
    });
    return wfd;
  };

  fsw.unwatch = (wfd) => { pol.unwatch(wfd); dirs.delete(wfd); fsw.close(wfd); };
})(this);
)JS";

ok64 JABCFswInstall() {
  JABC_API_OBJECT(fsw);
  JABC_API_FN(fsw, "init", JABCFswInit);
  JABC_API_FN(fsw, "dir", JABCFswDir);
  JABC_API_FN(fsw, "drain", JABCFswDrain);
  JABC_API_FN(fsw, "close", JABCFswClose);
  JABCExecute(JABC_FSW_JS);
  return OK;
}
