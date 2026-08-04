#include "JABC.hpp"
#include "pro.hpp"  //  HASHx uses sane/done; provided by the .cpp, not the header (§6)
#include "hash.hpp"
#include "heap.hpp"
#include "hit.hpp"
#include "index.hpp"
#include "pup.hpp"
#include "hunk.hpp"
#include "pack.hpp"
#include "ulog.hpp"
#include "cfold.hpp"

//  The container framework: per-(family,lane) prototypes whose verbs are bound
//  once to the native _heap_*/_hash_* leaves, plus the all-mmap constructors
//  (abc.ram / abc.mmap / abc.over).  A container IS a lane-typed view over a
//  JS-owned mapping; the cursor (watermark) rides on its ArrayBuffer; close()
//  drops the pin and lets GC munmap.  Embedded as a raw string literal.
static const char* JABC_CONT_JS = R"JS(
(function (g) {
  "use strict";
  const abc = g.abc, io = g.io, utf8 = g.utf8;

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
      const feed = abc["_heap_" + lane + "_feed"];
      const p = Object.create(M.A.prototype);
      p.push = M.pair
        ? function (k, v) { this.buffer.watermark = push(this, this.buffer.watermark, k, v); return this; }
        : function (v)    { this.buffer.watermark = push(this, this.buffer.watermark, v);    return this; };
      //  JS-106: bulk push of a contiguous entries view, one native call.
      p.feed = function (v) { this.buffer.watermark = feed(this, this.buffer.watermark, v); return this; };
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
    //  JS-055: size scratch off the RECORD's resolved size (a compressible
    //  object can inflate past the pack); `_dsc` is split, so ~2x `_bsc`.
    const SCRATCH_MIN = 1 << 16;
    P._scratch = function (need) {
      let n = need ? (need | 0) : SCRATCH_MIN;
      if (n < SCRATCH_MIN) n = SCRATCH_MIN;
      if (!this._bsc || this._bsc.byteLength < n) {
        this._bsc = new Uint8Array(n);     // base scratch (resolved bytes)
        this._dsc = new Uint8Array(2 * n); // delta scratch (split internally)
      }
    };
    //  JS-055: re-run `op` doubling scratch on the binding's NOROOM throw; any
    //  other error (ref-delta, corruption) propagates; capped against a loop.
    P._grow = function (need, op) {
      this._scratch(need);
      for (let tries = 0; tries < 40; tries++) {
        try { return op(); }
        catch (e) {
          if (!("" + e).includes("NOROOM")) throw e;
          this._scratch(this._bsc.byteLength * 2);
        }
      }
      throw "pack: scratch exhausted";
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
        //  JS-055: size off the base's declared resolved size; grow on NOROOM.
        base = this._grow(abc._pack_size(this, prevOff),
          () => abc._pack_resolve(this, prevOff, this._bsc, this._dsc));
        bo = prevOff;
      }
      //  delta-encode scratch must hold a delta no larger than `content`.
      this._scratch(content.length);
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
      //  JS-055: presize off the record's declared size, grow on NOROOM.
      const b = this._grow(abc._pack_size(this, this._rec),
        () => abc._pack_resolve(this, this._rec, this._bsc, this._dsc));
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
      //  JS-055: an object's inflated size can exceed the pack; the per-object
      //  max isn't known up front, so grow the scratch on NOROOM and re-scan.
      const idle = out.idle();
      const n = this._grow(0,
        () => abc._pack_scan(this, this.buffer.watermark | 0, idle, this._bsc, this._dsc));
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
      //  PTR-010: a rejected offset UNPOSITIONS the cursor — leaving the old
      //  `_rec` (or the initial -1) is what fed -1 to the size getter and
      //  read a byte below the mapping.  Callers that ignore `false` now see
      //  undefined, not a wild read.
      if (e < 0) { this._rec = -1; return false; }
      this._rec = off; this._read = e; return true;
    };
    //  Positioned? Every record getter is undefined on an unpositioned cursor.
    P._at = function () { return (this._rec | 0) >= 0; };
    Object.defineProperty(P, "count", { get() { return abc._pack_count(this); } });
    Object.defineProperty(P, "offset", { get() { return this._rec; } });
    Object.defineProperty(P, "type", {
      get() { return this._at() ? N2T[abc._pack_type(this, this._rec)] : undefined; }
    });
    Object.defineProperty(P, "size", {
      get() { return this._at() ? abc._pack_size(this, this._rec) : undefined; }
    });
    Object.defineProperty(P, "baseOffset", {
      get() {
        if (!this._at()) return undefined;
        const b = abc._pack_baseoff(this, this._rec); return b < 0 ? undefined : b;
      }
    });
    Object.defineProperty(P, "ref", {
      get() { return this._at() ? abc._pack_ref(this, this._rec) : undefined; }
    });
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

  //  CFOLD (DIS-082): one file's whole DAG history as a 'V' TLV blob in a u8
  //  buffer — the APPEND-ONLY weave.  Parsed zero-copy per call; fold()/merge()
  //  rewrite the WHOLE blob.  Commit ids are 16-char hex hashlet strings (the
  //  hi64 of the commit sha1) and every rev argument is one of them: visibility
  //  is STORED per commit, so no scope bitmaps and no token cursor exist.
  {
    const P = Object.create(Uint8Array.prototype);
    P.empty = function () { return abc._cfold_count(this, this.buffer.watermark | 0) === 0; };
    //  size = the 'E' INDEX entry count (inserts + tombs + chain terminators),
    //  not the visible-token count; tokens() answers that per rev.
    Object.defineProperty(P, "size",
      { get() { return abc._cfold_count(this, this.buffer.watermark | 0); } });
    Object.defineProperty(P, "commits",
      { get() { return abc._cfold_commits(this, this.buffer.watermark | 0); } });
    //  builders rewrite THIS buffer; base is another CFOLD container (or null
    //  for the first revision).  blob is file bytes (Uint8Array or Buf), and
    //  `ancestors` is the commit's whole causal closure (itself excluded).
    const bytesOf = (x) => (x && typeof x.data === "function") ? x.data() : x;
    P.fold = function (base, blob, ext, hash, ancestors) {
      const bl = base ? (base.buffer.watermark | 0) : 0;
      this.buffer.watermark = abc._cfold_next(this, base || null, bl,
        bytesOf(blob), ext, hash, ancestors || []);
      return this;
    };
    //  a merge carries no content: ONE already-folded weave in, one commit
    //  record out with the intersected ignore-set.
    P.merge = function (base, hash, ancestors) {
      this.buffer.watermark = abc._cfold_merge(this, base,
        base.buffer.watermark | 0, hash, ancestors || []);
      return this;
    };
    P.alive = function (out) {
      out.fed(abc._cfold_alive(this, this.buffer.watermark | 0, out.idle()));
      return out;
    };
    P.produce = function (rev, out) {
      out.fed(abc._cfold_produce(this, this.buffer.watermark | 0, rev, out.idle()));
      return out;
    };
    //  which commit inserted the token at body offset `off` -> its hashlet.
    P.blame = function (off) {
      return abc._cfold_blame(this, this.buffer.watermark | 0, off | 0);
    };
    //  the per-token cursor (the iteration API): rewind(rev) arms a JS-owned
    //  cursor, next() steps once and sets this.tok — text (a view into this
    //  blob), tag, off/end (BODY offsets: feed off to blame()), alive.
    P.rewind = function (rev) {
      this._rev = rev;
      this._cur = new Uint32Array(
          abc._cfold_itermem(this, this.buffer.watermark | 0));
      return this;
    };
    P.next = function () {
      const t = abc._cfold_step(this, this.buffer.watermark | 0,
                                this._rev, this._cur);
      if (!t) return false;
      this.tok = t;
      return true;
    };
    //  diff from-rev -> to-rev (commit hashlets) as HUNK records appended into
    //  a HUNK container `hunk` (toks carry the per-token diff side); read/render
    //  it with the HUNK cursor (next()/uri/text/toks/plain/color/html).
    P.emitDiff = function (from, to, name, navver, hunk) {
      hunk.buffer.watermark = abc._cfold_emitdiff(this, this.buffer.watermark | 0,
        name || "", navver || "", from, to, hunk, hunk.buffer.watermark | 0);
      return hunk;
    };
    P.emitFull = function (from, to, name, scheme, navver, hunk) {
      hunk.buffer.watermark = abc._cfold_emitfull(this, this.buffer.watermark | 0,
        name || "", scheme || "", navver || "", from, to, hunk, hunk.buffer.watermark | 0);
      return hunk;
    };
    PROTO["CFOLD"] = P;
  }

  const isLog = (f) => f === "HUNK" || f === "ULOG" || f === "CFOLD";   // u8-backed

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
    const b = c.buffer;
    let trim = -1;
    if (b._path) {                                  // booked: trim file to live size
      const M = LANE[c.lane];
      //  a PACK book (git.pack.book) has no .lane: it's a u8 log written
      //  start-to-end, so trim to the write head (watermark), no lane scaling.
      trim = M ? (c.size | 0) * M.w * M.A.BYTES_PER_ELEMENT : (b.watermark | 0);
    }
    //  ABC-020: detach FIRST (msync would materialize the buffer and defeat
    //  transfer's move) so stale views read empty, then flush+trim+release the
    //  fd+mapping NOW off the moved husk; double close is a harmless no-op.
    //  ABC-023: a read-only map holds no fd and no slot — close just detaches
    //  the views here, the pages go back at GC; _munmap finds nothing, no error.
    let husk;
    try { husk = b.transfer(); } catch (e) { return; }
    const view = new Uint8Array(husk);
    try { io._msync(view); } catch (e) {}
    if (trim >= 0) io._truncate(b._path, trim);
    io._munmap(view);
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
  //  JAB-020: `git.pack` is BOTH the container namespace (ram/over/mmap/book,
  //  unchanged) and the one-call stream ingest: git.pack(fd, buf, shard, opts)
  //  repacks a whole fetch into `<shard>/NNNNNNNNNN.keeper` logs inside libdog
  //  and returns its stats — no pack byte ever reaches the JS heap.  `buf` is
  //  a Buf (its DATA is what the pkt-line reader already ate, its `_data` /
  //  `_idle` come back advanced); `opts.index` is the caller's wh128 region.
  const pack = (fd, buf, shard, opts) =>
    abc._pack_repack(fd, buf, shard, opts || {});
  Object.assign(pack, {
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
  });
  //  git.tree(bytes[, cb]) — JS-028.  A PULL cursor over a git tree blob
  //  `(<mode> <name>\0<20-byte sha>)*`: ALL cursor state lives in JS (rule #4),
  //  the native leaf abc._git_tree_next drains exactly ONE entry per call (over
  //  dog/git GITu8sDrainTree) and reports {mode, nameStart, nameEnd, sha,
  //  nextOff}.  .next() advances and exposes .mode (octal incl. 0o160000
  //  gitlinks), .name (zero-copy Uint8Array subarray of the entry name), .str
  //  (utf8.Decode of .name), .sha (40-hex); false at end.  With a callback,
  //  drive the cursor in-frame (io.readdir style), one cb(entry) per entry.
  class GitTree {
    constructor(bytes) { this._b = bytes; this._off = 0; this._e = null; }
    next() {
      const e = abc._git_tree_next(this._b, this._off);
      if (e === null) { this._e = null; return false; }
      this._e = e; this._off = e.nextOff; return true;
    }
    get mode() { return this._e ? this._e.mode : undefined; }
    get sha()  { return this._e ? this._e.sha : undefined; }
    //  zero-copy name span over the SOURCE bytes (positions, not a copy).
    get name() {
      return this._e ? this._b.subarray(this._e.nameStart, this._e.nameEnd) : undefined;
    }
    get str() { return this._e ? utf8.Decode(this.name) : undefined; }
  }
  const gitTree = (bytes, cb) => {
    const t = new GitTree(bytes);
    if (cb == null) return t;
    while (t.next()) cb(t);            // in-frame, never stashed (rule #4)
    return undefined;
  };

  g.git = {
    pack,
    tree: gitTree,
    GitTree,
    //  parseCommit(bytes) — JS-028.  Eager {tree, parents[], foster[], author,
    //  committer, body} over dog/git GITu8sDrainCommit + GITu8sCommitTree (no
    //  manual framing in JS); commit objects are small so eager is fine.  tree /
    //  parents / foster are 40-hex strings; author/committer/body are strings.
    parseCommit: (bytes) => abc._git_parse_commit(bytes),
    //  delta.apply(base, delta, out): reconstruct a delta target into out (Buf)
    //  delta.encode(base, target, out): JS-036 twin — append the delta stream
    //  into out (Buf); -1 on DELTFAIL so the caller stores raw instead.
    delta: {
      apply: (base, delta, out) => {
        const n = abc._delt_apply(base, delta, out.idle(), 0);
        out.fed(n);
        return n;
      },
      encode: (base, target, out) => {
        const n = abc._delt_encode(base, target, out.idle(), 0);
        if (n < 0) return -1;
        out.fed(n);
        return n;
      },
    },
  };

  //  abc.index(lane, {dir, ext}) — a HANDLE on a dog Pup stack (DOG-027).
  //  The LSM itself lives in C: `<dir>` holds the immutable oldest-first runs
  //  (`<10-RON64><ext>`) plus the `<ext>` memtable, and every seal ends in the
  //  1/8 ladder, so nothing here knows or decides when a compaction happens.
  //
  //  This INVERTS API.md rule #4 for this subsystem, deliberately: the state is
  //  C-side (a kv64b dict behind the handle) and JS marshals only.  `dir` and
  //  `ext` ride the object as plain JS strings and go back down on every call
  //  that touches the filesystem.
  //
  //  put / commit / get / range / seek / count / run / drop / close — that IS
  //  the surface.  `runs`, `_seq`, `mem`, `flush` and `compact` are GONE: the
  //  run naming, the memtable, the sort and the k-way merge are one C layer.
  //  The family writes its marker row and calls commit(); run(i)/drop(i) are
  //  the audit path for a run whose marker is missing.
  //
  //  Lanes: kv64 (keyed: newest wins per key), wh128 ((key,val) rows, point on
  //  key) and u64 (scalar).  Reopening a dir written by an older jab works:
  //  padStart-named runs sort before every ron64 name, which is their correct
  //  age slot, and the C scan loads whatever matches `<seqno><ext>`.
  const IDX = {
    kv64:  { pair: true  },
    wh128: { pair: true  },
    u64:   { pair: false },
  };
  abc.index = (lane, opts) => {
    const M = IDX[lane];
    if (!M) throw "abc.index: unsupported lane " + lane + " (u64 | kv64 | wh128)";
    opts = opts || {};
    const dir = opts.dir;
    if (dir == null) throw "abc.index: an index needs a dir";
    const ext = opts.ext || ("." + lane);
    io.mkdir(dir);
    const L = (verb) => abc["_pup_" + lane + "_" + verb];
    const _put = L("put"), _commit = L("commit"), _get = L("get"),
          _range = L("range"), _seek = L("seek"), _next = L("next"),
          _count = L("count"), _run = L("run"), _drop = L("drop"),
          _close = L("close");

    const idx = { lane, dir, ext, _h: L("open")(dir, ext, opts.mode || "rw") };

    //  put: append to the memtable; a full page seals a run + runs the ladder.
    idx.put = M.pair
      ? function (k, v) { _put(this._h, this.dir, this.ext, BigInt(k), BigInt(v)); return this; }
      : function (v)    { _put(this._h, this.dir, this.ext, BigInt(v)); return this; };
    //  JS-106: bulk put of a contiguous entries view (pack.scan's shape).
    idx.feed = M.pair
      ? function (view) { for (let i = 0; i + 1 < view.length; i += 2) this.put(view[i], view[i + 1]); return this; }
      : function (view) { for (let i = 0; i < view.length; i++) this.put(view[i]); return this; };

    //  commit: the family put its marker row first; C collapses, seals, and
    //  compacts.  The rename is atomic, so a crash leaves no half-run.
    idx.commit = function () { _commit(this._h, this.dir, this.ext); return this; };

    //  point lookup, newest wins (the memtable shadows the runs).
    idx.get = function (k) { return _get(this._h, BigInt(k)); };

    //  ordered [lo, hi) scan through an in-frame cb; "enough"/false stops it.
    idx.range = function (lo, hi, cb) { _range(this._h, BigInt(lo), BigInt(hi), cb); return this; };
    //  prefix(p, lowBits) == range [p, p + 2^lowBits) — the keeper/spot block.
    idx.prefix = function (p, lowBits, cb) {
      const lo = BigInt(p);
      return this.range(lo, lo + (1n << BigInt(lowBits | 0)), cb);
    };

    //  seek(k): a merged pull cursor at the first row >= k.  The cursor state
    //  is the handle's (one at a time); .next() pulls ONE merged row.
    idx.seek = function (k) {
      const H = this._h;
      _seek(H, BigInt(k));
      const cur = { key: undefined, val: undefined, entry: undefined };
      cur.next = M.pair
        ? function () {
            const e = _next(H);
            if (e === undefined) { this.key = this.val = this.entry = undefined; return false; }
            this.key = e[0]; this.val = e[1]; this.entry = e;
            return true;
          }
        : function () {
            const v = _next(H);
            if (v === undefined) { this.key = this.val = this.entry = undefined; return false; }
            this.key = this.val = this.entry = v;
            return true;
          };
      return cur;
    };

    //  count: committed runs (the memtable is not one).
    Object.defineProperty(idx, "count", { get() { return _count(this._h); } });

    //  run(i): a read-only lane-typed view of run i, oldest-first — the
    //  family's marker audit.  It borrows the C mapping: drop/close frees it.
    idx.run = function (i) {
      const u = _run(this._h, i);
      return new BigUint64Array(u.buffer, u.byteOffset, (u.byteLength / 8) | 0);
    };
    //  drop(i): unlink a markerless run; drop() drops them all (re-derive).
    idx.drop = function (i) {
      if (i === undefined) _drop(this._h, this.dir, this.ext);
      else _drop(this._h, this.dir, this.ext, i);
      return this;
    };
    //  close: munmap the runs + the memtable, free the dict.
    idx.close = function () { _close(this._h); return this; };

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
  JABCPupInstall(abc);
  JABCHunkInstall(abc);
  JABCPackInstall(abc);
  JABCUlogInstall(abc);
  JABCCfoldInstall(abc);
  JABCExecute(JABC_CONT_JS);
  return OK;
}
