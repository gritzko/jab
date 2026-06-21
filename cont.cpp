#include "JABC.hpp"
extern "C" {
#include "abc/PRO.h"  //  HASHx uses sane/done; provided by the .cpp, not the header (§6)
}
#include "hash.hpp"
#include "heap.hpp"
#include "hit.hpp"
#include "index.hpp"
#include "hunk.hpp"
#include "pack.hpp"
#include "ulog.hpp"
#include "weave.hpp"

//  The container framework: per-(family,lane) prototypes whose verbs are bound
//  once to the native _heap_*/_hash_* leaves, plus the all-mmap constructors
//  (abc.ram / abc.mmap / abc.over).  A container IS a lane-typed view over a
//  JS-owned mapping; the cursor (watermark) rides on its ArrayBuffer; close()
//  drops the pin and lets GC munmap.  Embedded as a raw string literal.
static const char* JABC_CONT_JS = R"JS(
(function (g) {
  "use strict";
  const abc = g.abc, io = g.io;

  //  lane -> { typed-array ctor, JS elems per slot, pair?, bigint? }
  const LANE = {
    u8:    { A: Uint8Array,     w: 1, pair: false, big: false },
    u16:   { A: Uint16Array,    w: 1, pair: false, big: false },
    u32:   { A: Uint32Array,    w: 1, pair: false, big: false },
    u64:   { A: BigUint64Array, w: 1, pair: false, big: true  },
    kv32:  { A: Uint32Array,    w: 2, pair: true,  big: false },
    kv64:  { A: BigUint64Array, w: 2, pair: true,  big: true  },
    wh64:  { A: BigUint64Array, w: 1, pair: false, big: true  },
    wh128: { A: BigUint64Array, w: 2, pair: true,  big: true, set: true },
    sha1:   { A: Uint8Array, w: 20, pair: false, big: false, blob: true },
    sha256: { A: Uint8Array, w: 32, pair: false, big: false, blob: true },
  };
  const PROTO = {};
  const laneOf = (fam) => fam.slice(4);   // strip "HEAP"/"HASH" (4 chars)

  for (const lane in LANE) {
    const M = LANE[lane];
    //  HEAP: push/pop/peek/size, cursor on buffer.watermark
    {
      const push = abc["_heap_" + lane + "_push"];
      const pop  = abc["_heap_" + lane + "_pop"];
      const p = Object.create(M.A.prototype);
      p.push = M.pair
        ? function (k, v) { this.buffer.watermark = push(this, this.buffer.watermark, k, v); return this; }
        : function (v)    { this.buffer.watermark = push(this, this.buffer.watermark, v);    return this; };
      p.pop = function () {
        if (this.buffer.watermark === 0) return undefined;
        const r = pop(this, this.buffer.watermark);
        this.buffer.watermark--;
        return r;
      };
      p.peek = M.blob
        ? function () { return this.buffer.watermark ? this.subarray(0, M.w) : undefined; }
        : M.pair
        ? function () { return this.buffer.watermark ? [this[0], this[1]] : undefined; }
        : function () { return this.buffer.watermark ? this[0] : undefined; };
      Object.defineProperty(p, "size", { get() { return this.buffer.watermark; } });
      p.lane = lane;                                   // type tag for sort/merge
      const sort = abc["_sort_" + lane];               // QSORTx leaf, by lane Z
      p.sort = function () { sort(this, this.buffer.watermark); return this; };
      PROTO["HEAP" + lane] = p;
    }
    //  HASH: open-addressed table over the whole (zeroed, pow2) region
    {
      const put = abc["_hash_" + lane + "_put"];
      const get = abc["_hash_" + lane + "_get"];
      const del = abc["_hash_" + lane + "_del"];
      const p = Object.create(M.A.prototype);
      if (M.blob) {
        //  set of fixed-size byte blobs (sha1/sha256): key == the whole blob
        p.put = function (b) { put(this, b); return this; };
        p.get = function (b) { return get(this, b); };
        p.has = function (b) { return this.get(b) !== undefined; };
        p.del = function (b) { del(this, b); return this; };
      } else if (M.set) {
        //  set of (key,val) pairs (wh128): lookups supply both fields
        p.put = function (k, v) { put(this, k, v); return this; };
        p.get = function (k, v) { return get(this, k, v); };
        p.has = function (k, v) { return this.get(k, v) !== undefined; };
        p.del = function (k, v) { del(this, k, v); return this; };
      } else if (M.pair) {
        //  key -> val map (kv32/kv64)
        p.put = function (k, v) { put(this, k, v); return this; };
        p.get = function (k) { return get(this, k); };
        p.has = function (k) { return this.get(k) !== undefined; };
        p.del = function (k) { del(this, k); return this; };
      } else {
        //  set of scalars (get returns the value iff present)
        p.put = function (v) { put(this, v); return this; };
        p.get = function (v) { return get(this, v); };
        p.has = function (v) { return this.get(v) !== undefined; };
        p.del = function (v) { del(this, v); return this; };
      }
      PROTO["HASH" + lane] = p;
    }
  }

  //  HUNK: a u8-backed log of TLV 'H' records (not a lane).  The object IS the
  //  cursor — `next()` advances the read pos and the field getters reflect the
  //  current record (zero-copy views into the one buffer).
  {
    const EMPTY8 = new Uint8Array(0), EMPTY32 = new Uint32Array(0);
    const P = Object.create(Uint8Array.prototype);
    P.feed = function (uri, text, toks) {
      this.buffer.watermark = abc._hunk_feed(this, this.buffer.watermark,
        uri, text || EMPTY8, toks || EMPTY32);
      return this;
    };
    P.dogenize = function (src, ext, uri) {     // tokenize source -> append a hunk
      this.buffer.watermark = abc._hunk_dogenize(this, this.buffer.watermark,
        src, ext, uri || "");
      return this;
    };
    P.rewind = function () { this._read = 0; this._rec = -1; return this; };
    P.next = function () {
      const e = abc._hunk_next(this, this._read | 0, this.buffer.watermark | 0);
      if (e < 0) return false;
      this._rec = this._read | 0; this._read = e; return true;
    };
    Object.defineProperty(P, "uri",  { get() { return abc._hunk_uri(this, this._rec); } });
    Object.defineProperty(P, "text", { get() { return abc._hunk_text(this, this._rec); } });
    Object.defineProperty(P, "toks", { get() { return abc._hunk_toks(this, this._rec); } });
    Object.defineProperty(P, "verb", { get() { return abc._hunk_verb(this, this._rec); } });
    Object.defineProperty(P, "time", { get() { return abc._hunk_time(this, this._rec); } });
    //  `out` is an io.buf Buf (the buf.cpp cursor): render into its IDLE, then
    //  commit with fed().
    const render = (mode) => function (out) {
      out.fed(abc._hunk_render(this, this._rec, out.idle(), 0, mode));
      return out;
    };
    P.color = render(1);
    P.plain = render(2);
    P.html = render(3);
    PROTO["HUNK"] = P;
  }

  //  git.pack PROTO: an offset-addressed git pack log (u8).  feed returns the
  //  object's byte offset; seek/next address by offset only (a sha throws);
  //  delta records are surfaced (baseOffset/ref), not resolved.  JS-024: this
  //  is NOT an abc family — the `git` package below owns its constructors.
  const PACK_PROTO = (function () {
    const T2N = { commit: 1, tree: 2, blob: 3, tag: 4, "ofs-delta": 6, "ref-delta": 7 };
    const N2T = { 1: "commit", 2: "tree", 3: "blob", 4: "tag", 6: "ofs-delta", 7: "ref-delta" };
    const EMPTY8 = new Uint8Array(0);
    const P = Object.create(Uint8Array.prototype);
    //  Lazily size caller-owned scratch (base + delta) off the log's own
    //  byte length — an upper bound for any single object/delta in it.  The
    //  dog/git feed/resolve own all format logic; the binding only marshals.
    P._scratch = function () {
      if (!this._bsc) {
        const n = this.byteLength;
        this._bsc = new Uint8Array(n);   // base scratch (resolved bytes)
        this._dsc = new Uint8Array(2 * n); // delta scratch (split internally)
      }
    };
    P.header = function () {
      this.buffer.watermark = abc._pack_header(this, 0, 0);
      this._count = 0; this._read = 12; this._rec = -1;
      return this;
    };
    P.feed = function (type, content, prevOff, out) {
      const off = this.buffer.watermark;
      const t = (typeof type === "number") ? type : T2N[type];
      //  Resolve the base to its full bytes by offset (the read path); the
      //  dog/git writer decides OFS_DELTA vs raw from base+content.
      let base = EMPTY8, bo = -1;
      if (prevOff != null && prevOff >= 0) {
        this._scratch();
        base = abc._pack_resolve(this, prevOff, this._bsc, this._dsc);
        bo = prevOff;
      }
      this._scratch();
      this.buffer.watermark = abc._pack_feed(this, off, t, content, base, bo, this._dsc);
      this._count = (this._count | 0) + 1;
      //  GIT-010 index-on-append: if given an `out` entry sink (a Buf), drop
      //  the just-fed object's (sha->offset) wh128 entry in — git-sha the
      //  content we already hold, no resolve.  The pack-log owns no index.
      if (out != null) {
        const idle = out.idle();
        const n = abc._pack_feed_emit(t, content, off, idle, 0);
        out.fed(n);
      }
      return off;
    };
    //  resolve(out): chase this record's delta chain to full bytes via the
    //  dog/git resolver, append them to the `out` Buf.
    P.resolve = function (out) {
      this._scratch();
      const b = abc._pack_resolve(this, this._rec, this._bsc, this._dsc);
      out.feed(b);
      return out;
    };
    //  scan(out): GIT-010 index-EMIT — walk the WHOLE pack, resolve+git-sha
    //  each object, and drop one wh128 `(key=hashlet60|type, val=offset)`
    //  entry per object into the `out` Buf's IDLE via the dog/git PIDXScan.
    //  Advances out by n*16 bytes and returns a ZERO-COPY BigUint64Array view
    //  (n*2 u64s: key,val,key,val,...) over the bytes just written — the
    //  caller pipes them into an abc.index wh128 lane (idx.put(key, val)).
    //  The pack-log owns NO index: sort/merge/persist/query is the caller's.
    P.scan = function (out) {
      this._scratch();
      const idle = out.idle();
      const n = abc._pack_scan(this, this.buffer.watermark | 0, idle, this._bsc, this._dsc);
      out.fed(n * 16);                       // n wh128 entries (16 B each)
      return new BigUint64Array(idle.buffer, idle.byteOffset, n * 2);
    };
    P.finish = function () { abc._pack_header(this, 0, this._count | 0); return this; };
    P.rewind = function () { this._read = 12; this._rec = -1; return this; };
    P.next = function () {
      const e = abc._pack_next(this, this._read | 0, this.buffer.watermark | 0);
      if (e < 0) return false;
      this._rec = this._read | 0; this._read = e; return true;
    };
    P.seek = function (off) {
      if (typeof off !== "number") throw "PACK addresses by offset, not sha";
      const e = abc._pack_next(this, off, this.buffer.watermark | 0);
      if (e < 0) return false;
      this._rec = off; this._read = e; return true;
    };
    Object.defineProperty(P, "count", { get() { return abc._pack_count(this); } });
    Object.defineProperty(P, "offset", { get() { return this._rec; } });
    Object.defineProperty(P, "type", { get() { return N2T[abc._pack_type(this, this._rec)]; } });
    Object.defineProperty(P, "size", { get() { return abc._pack_size(this, this._rec); } });
    Object.defineProperty(P, "baseOffset", {
      get() { const b = abc._pack_baseoff(this, this._rec); return b < 0 ? undefined : b; }
    });
    Object.defineProperty(P, "ref", { get() { return abc._pack_ref(this, this._rec); } });
    P.inflate = function (out) {
      out.fed(abc._pack_inflate(this, this._rec, out.idle(), 0));
      return out;
    };
    return P;
  })();

  //  ULOG: append-only (ts,verb,uri) text log, no index.  feed moves the
  //  watermark; seek* are pure offset->offset scans (fwd, or rev from .end).
  {
    const P = Object.create(Uint8Array.prototype);
    P.feed = function (verb, uri, ts) {
      let t = (ts == null) ? abc._ulog_now() : BigInt(ts);
      if (this._lastTs != null && t <= this._lastTs) t = this._lastTs + 1n;  // monotonic
      this._lastTs = t;
      const off = this.buffer.watermark;
      this.buffer.watermark = abc._ulog_feed(this, off, verb, uri, t);
      return off;
    };
    P.rewind = function () { this._read = 0; this._rec = -1; return this; };
    P.next = function () {
      const e = abc._ulog_next(this, this._read | 0, this.buffer.watermark | 0);
      if (e < 0) return false;
      this._rec = this._read | 0; this._read = e; return true;
    };
    P.seek = function (off) {
      if (typeof off !== "number") throw "ULOG addresses by offset";
      const e = abc._ulog_next(this, off, this.buffer.watermark | 0);
      if (e < 0) return false;
      this._rec = off; this._read = e; return true;
    };
    Object.defineProperty(P, "end", { get() { return this.buffer.watermark; } });
    Object.defineProperty(P, "after", { get() { return this._read; } });
    Object.defineProperty(P, "offset", { get() { return this._rec; } });
    Object.defineProperty(P, "time", { get() { return abc._ulog_time(this, this._rec); } });
    Object.defineProperty(P, "verb", { get() { return abc._ulog_verb(this, this._rec); } });
    Object.defineProperty(P, "uri", { get() { return abc._ulog_uri(this, this._rec); } });
    const mkseek = (leaf, rev) => function (off, arg) {
      const o = abc[leaf](this, off, this.buffer.watermark | 0, arg, rev);
      if (o >= 0) this.seek(o);            // position cursor on a hit
      return o;
    };
    P.seekVerb = mkseek("_ulog_seekVerb", false);
    P.seekVerbRev = mkseek("_ulog_seekVerb", true);
    P.seekTime = mkseek("_ulog_seekTime", false);
    P.seekTimeRev = mkseek("_ulog_seekTime", true);
    P.seekURI = mkseek("_ulog_seekURI", false);
    P.seekURIRev = mkseek("_ulog_seekURI", true);
    PROTO["ULOG"] = P;
  }

  //  WEAVE: one file's whole DAG history as a 'W' TLV blob in a u8 buffer.
  //  Parsed zero-copy per call; fold()/merge() rewrite the WHOLE blob.  Commit
  //  ids are 16-char hex hashlet strings (the hi64 of the commit sha1).  The
  //  token cursor (rewind()/next() + getters) is WEAVEStep; fold = WEAVENext.
  {
    const P = Object.create(Uint8Array.prototype);
    P.empty = function () { return abc._weave_count(this, this.buffer.watermark | 0) === 0; };
    Object.defineProperty(P, "size",
      { get() { return abc._weave_count(this, this.buffer.watermark | 0); } });
    Object.defineProperty(P, "commits",
      { get() { return abc._weave_commits(this, this.buffer.watermark | 0); } });
    //  builders rewrite THIS buffer; base/a/b are other WEAVE containers (or
    //  null base for the first revision).  blob is file bytes (Uint8Array or Buf).
    const bytesOf = (x) => (x && typeof x.data === "function") ? x.data() : x;
    P.fold = function (base, blob, ext, hash) {
      const bl = base ? (base.buffer.watermark | 0) : 0;
      this.buffer.watermark = abc._weave_next(this, base || null, bl, bytesOf(blob), ext, hash);
      return this.rewind();
    };
    P.merge = function (a, b, hash) {
      this.buffer.watermark = abc._weave_merge(this, a, a.buffer.watermark | 0,
        b, b.buffer.watermark | 0, hash);
      return this.rewind();
    };
    P.rewind = function () {
      if (!this._cur) this._cur = new Uint32Array(6); else this._cur.fill(0);
      this._tok = null;
      return this;
    };
    P.next = function () {
      if (!this._cur) this._cur = new Uint32Array(6);
      const t = abc._weave_step(this, this.buffer.watermark | 0, this._cur);
      this._tok = (t === false) ? null : t;
      return t !== false;
    };
    const cur = (k) => function () { return this._tok ? this._tok[k] : undefined; };
    Object.defineProperty(P, "tag",       { get: cur("tag") });
    Object.defineProperty(P, "tokText",   { get: cur("text") });
    Object.defineProperty(P, "hasIn",     { get: cur("hasIn") });
    Object.defineProperty(P, "inserter",  { get: cur("inserter") });
    Object.defineProperty(P, "rms",       { get: cur("rms") });
    Object.defineProperty(P, "anchor",    { get: cur("anchor") });
    Object.defineProperty(P, "hasAnchor", { get: cur("hasAnchor") });
    //  scope: active-commit bitmap (Uint8Array) over an array of hashlet strings.
    P.scope = function (activeHashlets) {
      return abc._weave_scope(this, this.buffer.watermark | 0, activeHashlets || []);
    };
    P.alive = function (out) {
      out.fed(abc._weave_alive(this, this.buffer.watermark | 0, out.idle()));
      return out;
    };
    P.produce = function (scope, out) {
      out.fed(abc._weave_produce(this, this.buffer.watermark | 0, scope, out.idle()));
      return out;
    };
    //  diff from-scope -> to-scope as HUNK records appended into a HUNK
    //  container `hunk` (toks carry the per-token diff side); read/render it
    //  with the HUNK cursor (next()/uri/text/toks/plain/color/html).
    P.emitDiff = function (from, to, name, navver, hunk) {
      hunk.buffer.watermark = abc._weave_emitdiff(this, this.buffer.watermark | 0,
        name || "", navver || "", from, to, hunk, hunk.buffer.watermark | 0);
      return hunk;
    };
    P.emitFull = function (from, to, name, scheme, navver, hunk) {
      hunk.buffer.watermark = abc._weave_emitfull(this, this.buffer.watermark | 0,
        name || "", scheme || "", navver || "", from, to, hunk, hunk.buffer.watermark | 0);
      return hunk;
    };
    //  N-way conflict render into out: groups is an array of side scopes
    //  ([ours, theirs, ...]); divergent regions framed <<<< |||| >>>>.
    P.merged = function (groups, out) {
      out.fed(abc._weave_merged(this, this.buffer.watermark | 0, groups, out.idle()));
      return out;
    };
    PROTO["WEAVE"] = P;
  }

  const isLog = (f) => f === "HUNK" || f === "ULOG" || f === "WEAVE";   // u8-backed

  function build(family, u8) {
    const proto = PROTO[family];
    if (!proto) throw "abc: unknown family " + family;
    let v;
    if (isLog(family)) {                // u8-backed log: use the byte view directly
      v = u8;
      Object.setPrototypeOf(v, proto);
      v._read = 0;
      v._rec = -1;
    } else {
      const M = LANE[laneOf(family)];
      if (!M) throw "abc: unknown family " + family;
      v = new M.A(u8.buffer, u8.byteOffset, (u8.byteLength / M.A.BYTES_PER_ELEMENT) | 0);
      Object.setPrototypeOf(v, proto);   // real typed array + the family's verbs
    }
    v.buffer._map = u8;                // pin the mapping (munmap on GC)
    v.buffer.watermark = 0;
    return v;
  }
  const bytes = (family, slots) => {
    if (isLog(family)) return slots;              // u8 bytes
    const M = LANE[laneOf(family)];
    return slots * M.w * M.A.BYTES_PER_ELEMENT;
  };

  abc.ram  = (family, slots) => build(family, io._ram(bytes(family, slots)));
  abc.mmap = (family, path, mode, slots) => {
    mode = mode || "rw";
    return build(family, io._mmap(path, mode, mode === "c" ? bytes(family, slots || 0) : 0));
  };
  abc.over = (family, ta) => build(family,
    (ta instanceof Uint8Array) ? ta : new Uint8Array(ta.buffer, ta.byteOffset, ta.byteLength));

  //  book: a file-backed output sized to an upper bound (cheap — sparse), to
  //  be filled (e.g. by merge) and trimmed to the live size on close.
  abc.book = (family, path, slots) => {
    const c = build(family, io._mmap(path, "c", bytes(family, slots)));
    c.buffer._path = path;
    return c;
  };
  abc.close = (c) => {
    try { io._msync(c); } catch (e) {}
    const b = c.buffer;
    if (b._path) {                                  // booked: trim file to live size
      const M = LANE[c.lane];
      //  a PACK book (git.pack.book) has no .lane: it's a u8 log written
      //  start-to-end, so trim to the write head (watermark), no lane scaling.
      if (M) io._truncate(b._path, (c.size | 0) * M.w * M.A.BYTES_PER_ELEMENT);
      else   io._truncate(b._path, (b.watermark | 0));
    }
    b._map = null;
  };

  //  HIT bulk ops over SORTED runs.  Inputs are containers (each carries its
  //  .lane and .size); the lane is read off the operands (no type arg), all
  //  must agree.  Without `out` a fresh lane-typed run is returned; with `out`
  //  (a container sized to >= Sum of inputs) the result is written in place and
  //  out.size is set, so it can be trimmed on close (abc.book) and re-merged.
  const kway = (pfx, op, inputs, out) => {
    if (!inputs.length) throw "abc." + op + ": no inputs";
    const lane = inputs[0].lane;
    if (!lane) throw "abc." + op + ": inputs need a lane (use abc.ram/over)";
    const M = LANE[lane];
    const runs = inputs.map((c) => {
      if (c.lane !== lane) throw "abc." + op + ": lane mismatch";
      return c.subarray(0, (c.size | 0) * M.w);     // live region only
    });
    if (out) {
      if (out.lane !== lane) throw "abc." + op + ": out lane mismatch";
      out.buffer.watermark = abc[pfx + lane](runs, out);
      return out;
    }
    const u8 = abc[pfx + lane](runs);
    return new M.A(u8.buffer, u8.byteOffset, (u8.byteLength / M.A.BYTES_PER_ELEMENT) | 0);
  };
  abc.merge = (inputs, out) => kway("_merge_", "merge", inputs, out);
  abc.intersect = (inputs, out) => kway("_isect_", "intersect", inputs, out);

  //  WEAVE token identity/anchor hash: hashlet(RAPHash(commit-id ++ ordinal)).
  abc.weaveIdHash = (hash, ord) => abc._weave_idhash(hash, ord | 0);

  //  git: the git/pack-log package (JS-024).  `git.pack` is the offset-pure
  //  PACK container (migrated, hard cutover, off the old abc "PACK" family);
  //  `git.delta.apply` is the delta op (migrated off the old global `delt`).
  //  The native leaves (abc._pack_* / abc._delt_apply) are UNCHANGED — `git`
  //  only re-homes the JS surface (the SAME PACK_PROTO + delta marshalling).
  //  A PACK is a u8-backed log: wrap the byte view, pin the mapping (munmap on
  //  GC), skip the 12-byte header (_read = 12), cursor at -1.
  const packBuild = (u8) => {
    const v = u8;
    Object.setPrototypeOf(v, PACK_PROTO);
    v._read = 12;                    // PACK skips the 12-byte header
    v._rec = -1;
    v.buffer._map = u8;              // pin the mapping (munmap on GC)
    v.buffer.watermark = 0;
    return v;
  };
  const pack = {
    ram:  (slots) => packBuild(io._ram(slots)),
    over: (ta) => packBuild((ta instanceof Uint8Array) ? ta
                            : new Uint8Array(ta.buffer, ta.byteOffset, ta.byteLength)),
    mmap: (path, mode, slots) => {
      mode = mode || "rw";
      return packBuild(io._mmap(path, mode, mode === "c" ? (slots || 0) : 0));
    },
    //  book: a sparse file-backed log sized to an upper bound (see abc.book);
    //  msync + unpin on abc.close (a PACK carries no .lane, so close skips the
    //  lane-typed trim — a PACK file is written start-to-end, no truncation).
    book: (path, slots) => {
      const c = packBuild(io._mmap(path, "c", slots));
      c.buffer._path = path;
      return c;
    },
  };
  g.git = {
    pack,
    //  delta.apply(base, delta, out): reconstruct a delta target into out (Buf)
    delta: {
      apply: (base, delta, out) => {
        const n = abc._delt_apply(base, delta, out.idle(), 0);
        out.fed(n);
        return n;
      },
    },
  };

  //  abc.index(lane, {dir, ext, mem}) — a mmap LSM index (JS-022): a stack of
  //  immutable, oldest-first sorted runs (.runs) + a memtable.  The write path
  //  is PURE JS over the existing leaves (HEAP sort + abc.merge/book/close);
  //  the read path (get / range / prefix) and compact() ride the 3 native
  //  index leaves (_findge_ / _seekrange_ / _compact_).  Queries STREAM hits
  //  through an in-frame cb (rule #4); cb's return is a stop signal.
  //
  //  Two run backings, chosen by opts.dir (JS-side branch — the leaves are
  //  backing-agnostic, taking typed-array views):
  //    * ON-DISK (opts.dir PRESENT): runs are abc.book files in <dir>; flush
  //      books + msync-trims + io.rename + re-opens RO; compact lands the merged
  //      run on disk; an existing dir is re-opened (its run files loaded).
  //    * IN-MEMORY (opts.dir ABSENT): runs live in anonymous io.ram mappings —
  //      no files, no rename/re-open; flush merges the sorted memtable into a
  //      fresh RAM run, compact merges RAM->RAM.  Starts empty, gone at exit.
  //
  //  v1 deletes: the newest run shadows older at QUERY time (no tombstone
  //  drop).  Compaction's full-element dedup + 1/8 ladder collapses identical
  //  rows only — same-key/diff-val rows coexist (keyed compaction is deferred).
  //  .range/.prefix keep the native _seekrange_ fast-drain (NO cross-run dedup;
  //  collapse happens at compaction).  .seek(needle) is a PURE-JS pull cursor:
  //  a k-way min-heap over each source's head (runs seeked via _findge_, plus
  //  the snapshotted memtable) with full-element newest-wins dedup, advancing
  //  ONE merged entry per .next() — ALL cursor state in JS (rule #4).
  //  Lanes: u64 (scalar) and wh128 ((key,val), point on key).
  const IDX = {
    u64:   { fam: "HEAPu64",   w: 1, pair: false },
    wh128: { fam: "HEAPwh128", w: 2, pair: true  },
  };
  abc.index = (lane, opts) => {
    const M = IDX[lane];
    if (!M) throw "abc.index: unsupported lane " + lane + " (u64 | wh128)";
    opts = opts || {};
    const dir = opts.dir;
    const onDisk = (dir != null);              // present => persisted file runs
    const ext = opts.ext || ("." + lane);
    const memSlots = (opts.mem | 0) || 4096;
    if (onDisk) io.mkdir(dir);

    const findge = abc["_findge_" + lane];
    const seekrange = abc["_seekrange_" + lane];
    const compact = abc["_compact_" + lane];

    const idx = {
      lane, dir, ext, w: M.w,
      onDisk,                      // backing: true=file runs, false=io.ram runs
      runs: [],                    // oldest-first; each a HEAPlane container
      mem: abc.ram(M.fam, memSlots),
      _seq: 0,
    };

    //  validate alignment on opening a run RO: byteLength % (w*BPE) === 0.
    const openRun = (path) => {
      const r = abc.mmap(M.fam, path, "r");
      const slot = M.w * r.BYTES_PER_ELEMENT;
      if ((r.byteLength % slot) !== 0)
        throw "abc.index: misaligned run " + path + " (" + r.byteLength + " % " + slot + ")";
      r.buffer.watermark = (r.byteLength / slot) | 0;   // live = whole file
      r._path = path;                                   // for unlink on compact
      return r;
    };

    //  load any pre-existing run files in the dir (sorted by name == seqno).
    //  IN-MEMORY indexes start empty each open (nothing to load).
    if (onDisk) {
      const sfx = ext;
      const files = io.readdir(dir).filter((f) => f.endsWith(sfx)).sort();
      for (const f of files) {
        idx.runs.push(openRun(dir + "/" + f));
        const n = parseInt(f, 10);
        if (n >= idx._seq) idx._seq = n + 1;
      }
    }

    const runPath = (seq) =>
      dir + "/" + String(seq).padStart(8, "0") + ext;

    //  put: append into the memtable (u64: one BigInt; wh128: key,val).
    idx.put = M.pair
      ? function (k, v) { this.mem.push(BigInt(k), BigInt(v)); return this; }
      : function (v)    { this.mem.push(BigInt(v));           return this; };

    //  flush: memtable.sort() -> abc.merge([mem]) into a fresh sorted+deduped
    //  run -> push.  ON-DISK: book a temp file, merge into it, close (msync +
    //  ftruncate), rename into the final run file, re-open RO.  IN-MEMORY: merge
    //  into an io.ram destination sized to mem.size; merge sets its watermark to
    //  the live count (no file, no close/rename/re-open).
    idx.flush = function () {
      if (this.mem.size === 0) return this;
      this.mem.sort();
      if (this.onDisk) {
        const seq = this._seq++;
        const tmp = this.dir + "/." + String(seq).padStart(8, "0") + this.ext + ".tmp";
        const out = abc.book(M.fam, tmp, this.mem.size);   // upper bound = mem size
        abc.merge([this.mem], out);                        // sorted+deduped run
        abc.close(out);                                    // msync + ftruncate
        const path = runPath(seq);
        io.rename(tmp, path);
        this.runs.push(openRun(path));
      } else {
        const out = abc.ram(M.fam, this.mem.size);         // upper bound = mem size
        abc.merge([this.mem], out);                        // sets out.watermark
        this._seq++;
        this.runs.push(out);                               // RAM run; GC munmaps
      }
      this.mem.buffer.watermark = 0;                       // empty the memtable
      return this;
    };

    //  compact: run the 1/8 ladder over .runs via _compact_<lane>.  The native
    //  leaf merges the youngest m runs into the destination; replace
    //  runs[len-m..] with that one run.  Cascades until IsCompact holds.
    //  ON-DISK: destination is a booked temp file, trimmed + renamed into place,
    //  the m sources unlinked + unpinned.  IN-MEMORY: destination is an io.ram
    //  mapping sized to sum(runs); the m source mappings are unpinned (GC
    //  munmaps).  The native leaf is backing-agnostic (typed-array views only).
    idx.compact = function () {
      while (this.runs.length >= 2) {
        const slices = this.runs.map((r) => r.subarray(0, (r.buffer.watermark | 0) * M.w));
        let total = 0;
        for (const s of slices) total += s.length / M.w;
        const seq = this._seq++;
        if (this.onDisk) {
          //  book the destination run, compact the youngest runs straight into
          //  its mapping, then trim + rename it into place.
          const tmp = this.dir + "/." + String(seq).padStart(8, "0") + this.ext + ".tmp";
          const book = abc.book(M.fam, tmp, total || 1);
          const [merged, m] = compact(slices, book);
          if (m < 2) { abc.close(book); io.unlink(tmp); this._seq--; break; }
          book.buffer.watermark = merged | 0;              // live = merged elems
          abc.close(book);                                 // msync + trim
          const path = runPath(seq);
          io.rename(tmp, path);
          for (let i = 0; i < m; i++) {                    // drop the m sources
            const old = this.runs.pop();
            if (old._path) io.unlink(old._path);
            old.buffer._map = null;                        // unpin (GC munmaps)
          }
          this.runs.push(openRun(path));
        } else {
          //  merge the youngest runs into a fresh RAM mapping (sum upper bound).
          const out = abc.ram(M.fam, total || 1);
          const [merged, m] = compact(slices, out);
          if (m < 2) { out.buffer._map = null; this._seq--; break; }
          out.buffer.watermark = merged | 0;               // live = merged elems
          for (let i = 0; i < m; i++) {                    // drop the m sources
            const old = this.runs.pop();
            old.buffer._map = null;                        // unpin (GC munmaps)
          }
          this.runs.push(out);
        }
      }
      return this;
    };

    //  snapshot the (live) query sources, newest-first: the sorted memtable
    //  slice (if non-empty) followed by the runs newest->oldest.  Sorting the
    //  memtable in place is idempotent for an already-sorted run.
    const querySources = () => {
      const src = [];
      if (idx.mem.size > 0) {
        idx.mem.sort();
        src.push(idx.mem.subarray(0, (idx.mem.buffer.watermark | 0) * M.w));
      }
      for (let i = idx.runs.length - 1; i >= 0; i--) {
        const r = idx.runs[i];
        src.push(r.subarray(0, (r.buffer.watermark | 0) * M.w));
      }
      return src;                                          // newest-first
    };

    //  point get: scan sources newest-first; FindGE the key-low needle, read
    //  the element, accept iff its key matches (keeper KEEPLookup pattern).
    //  Returns the value (u64: the scalar; wh128: the val for that key) or
    //  undefined.  Newest source wins (shadowing).
    idx.get = M.pair
      ? function (key) {
          key = BigInt(key);
          for (const s of querySources()) {
            const i = findge(s, key, 0n);                  // needle (key, 0)
            if (i * M.w < s.length && s[i * M.w] === key) return s[i * M.w + 1];
          }
          return undefined;
        }
      : function (v) {
          v = BigInt(v);
          for (const s of querySources()) {
            const i = findge(s, v);
            if (i < s.length && s[i] === v) return v;
          }
          return undefined;
        };

    //  range [lo, hi): SeekRange the heap of all sources to [lo,hi), drain it
    //  through cb.  u64: lo/hi scalars, cb(value).  wh128: lo/hi are KEYS, the
    //  range is [(lo,0), (hi,0)) over (key,val), cb([key,val]).
    idx.range = M.pair
      ? function (lo, hi, cb) {
          seekrange(querySources(), BigInt(lo), 0n, BigInt(hi), 0n, cb);
        }
      : function (lo, hi, cb) {
          seekrange(querySources(), BigInt(lo), BigInt(hi), cb);
        };

    //  prefix(p, lowBits, cb): range [p, p + 2^lowBits) — the keeper/spot
    //  fixed-prefix block (spot capo_seek_prefix: [prefix, prefix + 1<<24)).
    idx.prefix = function (p, lowBits, cb) {
      const lo = BigInt(p);
      const hi = lo + (1n << BigInt(lowBits | 0));
      return this.range(lo, hi, cb);
    };

    //  --- the seek cursor: a PURE-JS pull-based k-way merge (rule #4). ---
    //  Compare TWO heads (each a [s, i] = source view + element index) in lane
    //  sort order: u64 by the scalar, wh128 lexicographically by (key, val) —
    //  the exact ordering of <lane>sFindGE / the run typed-array views.  Used to
    //  order the min-heap AND to detect duplicates (a == b iff !lt(a,b)&&!lt(b,a)).
    const headLT = M.pair
      ? (sa, ia, sb, ib) => {
          const ka = sa[ia * 2], kb = sb[ib * 2];
          if (ka !== kb) return ka < kb;
          return sa[ia * 2 + 1] < sb[ib * 2 + 1];
        }
      : (sa, ia, sb, ib) => sa[ia] < sb[ib];

    //  seek(needle): position every source (each sorted run via _findge_, plus
    //  the snapshotted memtable) at the first element >= needle, then pull ONE
    //  merged entry per .next() in ascending order.  Sources carry their JS
    //  position; a tiny binary min-heap (indices into `srcs`) orders the current
    //  heads; full-element newest-wins dedup (sources are newest-first, so the
    //  first head equal to the just-emitted element wins and the rest are
    //  skipped).  ALL state is JS-owned (no held native cursor) — the only
    //  native call is the per-source _findge_ binary search at seek time.
    //
    //  Memtable consistency: querySources() sorts the memtable in place and
    //  snapshots its live slice as the NEWEST source, so a pending put is
    //  visible to the cursor exactly as it is to get/range — no auto-flush, no
    //  fork (the snapshot is a subarray view; further puts after seek are not
    //  reflected, matching the "snapshot on seek" contract).
    idx.seek = function (...needle) {
      const M_ = M;
      const sources = querySources();                // newest-first slices
      //  srcs[k] = { s: view, i: cursor index, n: element count }.  Position
      //  each at the first element >= needle via the native binary search.
      const srcs = [];
      for (const s of sources) {
        const n = (s.length / M_.w) | 0;
        const i = M_.pair ? findge(s, needle[0], needle[1] || 0n)
                          : findge(s, needle[0]);
        if (i < n) srcs.push({ s, i, n });
      }
      //  binary min-heap of indices into srcs, ordered by head element; ties
      //  break by source order (newest first) so dedup keeps the newest.
      const heap = [];                               // holds k = index in srcs
      const lt = (a, b) => {
        const A = srcs[a], B = srcs[b];
        if (headLT(A.s, A.i, B.s, B.i)) return true;
        if (headLT(B.s, B.i, A.s, A.i)) return false;
        return a < b;                                // equal heads: newer wins
      };
      const up = (c) => {
        while (c > 0) {
          const p = (c - 1) >> 1;
          if (!lt(heap[c], heap[p])) break;
          const t = heap[c]; heap[c] = heap[p]; heap[p] = t; c = p;
        }
      };
      const down = (p) => {
        const L = heap.length;
        for (;;) {
          let m = p, l = 2 * p + 1, r = l + 1;
          if (l < L && lt(heap[l], heap[m])) m = l;
          if (r < L && lt(heap[r], heap[m])) m = r;
          if (m === p) break;
          const t = heap[p]; heap[p] = heap[m]; heap[m] = t; p = m;
        }
      };
      const push = (k) => { heap.push(k); up(heap.length - 1); };
      for (let k = 0; k < srcs.length; k++) push(k);

      //  advance source k by one; re-heapify (or drop it if exhausted).
      const advance = (k) => {
        srcs[k].i++;
        if (srcs[k].i < srcs[k].n) { heap[0] = k; down(0); }
        else {                                       // exhausted: pop the root
          const last = heap.pop();
          if (heap.length) { heap[0] = last; down(0); }
        }
      };

      const cur = M_.pair ? { key: undefined, val: undefined, entry: undefined }
                          : { key: undefined, val: undefined, entry: undefined };
      const cursor = {
        get key()   { return cur.key; },
        get val()   { return cur.val; },
        get entry() { return cur.entry; },
        //  pull one merged entry; collapse all sources whose head equals it
        //  (newest-wins is automatic — they are identical full elements).
        //  Returns false past the last entry (cur fields go undefined).
        next() {
          if (heap.length === 0) {
            cur.key = cur.val = cur.entry = undefined;
            return false;
          }
          const k = heap[0], src = srcs[k];
          if (M_.pair) {
            const key = src.s[src.i * 2], val = src.s[src.i * 2 + 1];
            cur.key = key; cur.val = val; cur.entry = [key, val];
          } else {
            const v = src.s[src.i];
            cur.key = v; cur.val = v; cur.entry = v;
          }
          //  drop this element from every source that holds it (dup collapse).
          advance(k);
          while (heap.length) {
            const j = heap[0], t = srcs[j];
            const eq = M_.pair
              ? (t.s[t.i * 2] === cur.key && t.s[t.i * 2 + 1] === cur.val)
              : (t.s[t.i] === cur.key);
            if (!eq) break;
            advance(j);
          }
          return true;
        },
      };
      return cursor;
    };

    return idx;
  };
})(this);
)JS";

ok64 JABCContInstall() {
  JABC_API_OBJECT(abc);
  JABCHeapInstall(abc);
  JABCHashInstall(abc);
  JABCHitInstall(abc);
  JABCIndexInstall(abc);
  JABCHunkInstall(abc);
  JABCPackInstall(abc);
  JABCUlogInstall(abc);
  JABCWeaveInstall(abc);
  JABCExecute(JABC_CONT_JS);
  return OK;
}
