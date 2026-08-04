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

//  JAB-031: one packed record — i32 wd, u32 name length (both LE), name bytes.
//  Room is checked whole, so IDLE never holds a torn record.
//  JAB-032: wd is the real watch descriptor now, and it is SIGNED — FSWOVERFLOW
//  (-1) says the kernel dropped events, so the JS side drops every cache.
static ok64 JABCFswPack(i32 wd, u8cs name, void* ctx) {
  JABCFswSink* s = (JABCFswSink*)ctx;
  u32 len = (u32)u8csLen(name);
  u32 raw = (u32)wd;  //  two's complement on the wire; JS reads it signed
  if (u8bIdleLen(s->out) < sizeof(raw) + sizeof(len) + (size_t)len)
    return FSWNOROOM;
  u8sFeed32(u8bIdle(s->out), &raw);
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

//  fsw.dir(wfd, path) -> wd.  Non-recursive, one dir level.  JAB-032: the wd
//  names this dir in every drained record, so ONE wfd serves a whole tree.
static JABC_FN(JABCFswDir) {
  if (argc < 2) JABC_THROW("fsw.dir(wfd, path)");
  u64 wfd = 0;
  if (!JABCu64Of(&wfd, ctx, args[0], exception)) JABC_UNDEF;
  a_pad(u8, p, FILE_PATH_MAX_LEN);
  if (JABCPath(p, ctx, args[1], exception) != OK) {
    if (*exception) return JSValueMakeUndefined(ctx);
    JABC_THROW("fsw.dir: bad path");
  }
  i32 wd = 0;
  if (FSWDir((int)wfd, $path(p), &wd) != OK)
    JABC_THROW("fsw.dir: cannot watch that directory");
  return JSValueMakeNumber(ctx, (double)wd);
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

//  JAB-031: all per-watch state lives here.  JAB-032: FSWDir hands back a wd,
//  so ONE watcher fd carries the whole tree and the wd -> dir map does the
//  attribution — no more fd + pol handler + 64 KiB Buf per watched dir.
static const char* JABC_FSW_JS = R"JS(
(function (g) {
  "use strict";
  const fsw = g.fsw, io = g.io, pol = g.pol;
  const HDR = 8;                 // i32 wd + u32 name length, little-endian
  const dirs = new Map();        // wd -> watched dir (one shared watcher)
  const subs = new Map();        // wd -> handler
  let wfd = -1, buf = null, over = null;

  //  events were LOST (kernel queue overflow, or a Buf too small for the
  //  burst): nothing under this watcher is trustworthy, so every watched dir
  //  gets a bare rescan ("" name) unless fsw.onoverflow claimed the fact.
  const lost = () => {
    if (over) { over(); return; }
    dirs.forEach((d, w) => { const h = subs.get(w); if (h) h("", d); });
  };

  //  parse a drained Buf into [{wd, name}]; name is a BARE basename, and is
  //  "" on kqueue (no filename in the event) — a "rescan this dir" signal.
  //  wd is SIGNED: fsw.OVERFLOW (-1) means the kernel DROPPED events.
  fsw.OVERFLOW = -1;
  fsw.records = (buf) => {
    const b = buf.data ? buf.data() : buf;
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const out = [];
    for (let o = 0; o + HDR <= b.byteLength; ) {
      const wd = dv.getInt32(o, true), len = dv.getUint32(o + 4, true);
      if (o + HDR + len > b.byteLength) break;
      out.push({ wd: wd, name: g.utf8.Decode(b.subarray(o + HDR, o + HDR + len)) });
      o += HDR + len;
    }
    return out;
  };

  //  fsw.watch(dir, fn) -> wd; fn(name, dir) per event ("" name => rescan the
  //  dir).  Every watch rides ONE watcher fd, armed lazily on the first call.
  fsw.watch = (dir, fn) => {
    if (wfd < 0) {
      wfd = fsw.init();
      buf = io.buf(1 << 16);
      pol.watch(wfd, pol.IN, (fd) => {
        for (;;) {                       // drain to empty: one Buf, many bites
          let n = 0;
          //  a full Buf is the SAME fact as a kernel overflow — events were
          //  lost — so it takes the same path instead of escaping as a throw.
          try { n = fsw.drain(fd, buf.reset()); } catch (e) { lost(); break; }
          if (n === 0) break;
          const rows = fsw.records(buf);
          for (let i = 0; i < rows.length; i++) {
            const r = rows[i];
            if (r.wd === fsw.OVERFLOW) { lost(); continue; }
            const d = dirs.get(r.wd), h = subs.get(r.wd);
            if (h) h(r.name, d);
          }
        }
        return pol.IN;
      });
    }
    const wd = fsw.dir(wfd, dir);
    dirs.set(wd, dir);
    subs.set(wd, fn);
    return wd;
  };

  //  fsw.onoverflow(fn) — one callback for "the kernel dropped events";
  //  without it every watched dir gets a bare rescan ("" name) instead.
  fsw.onoverflow = (fn) => { over = fn; };

  //  fsw.unwatch(wd) — forget one dir.  The inotify watch itself stays (no
  //  FSWUndir, ABC-013), so events still arrive; they are simply unclaimed.
  fsw.unwatch = (wd) => { dirs.delete(wd); subs.delete(wd); };

  //  fsw.stop() — tear the shared watcher down (every watch dies with it).
  fsw.stop = () => {
    if (wfd < 0) return;
    pol.unwatch(wfd); fsw.close(wfd);
    wfd = -1; buf = null; dirs.clear(); subs.clear();
  };
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
