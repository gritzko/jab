"use strict";
// JS-022: abc.index(lane, {dir, ext, mem}) — a mmap LSM index over a stack of
// immutable sorted runs + a memtable.  Verbs: put / flush / compact / get
// (point) / range / prefix.  Queries stream hits through an IN-FRAME callback
// (rule #4) that returns a stop signal (mirror io.readdir(path, cb)).
//
// Two real lanes: wh128 (keeper puppy registry: (key,val), point on KEY) and
// u64 (spot trigram: scalar).  We assert POINT/RANGE/PREFIX against a plain-JS
// brute-force oracle (a sorted array doing the same lookups).
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const DIR = "/tmp/jabc_index_" + process.pid;

// --- a clean per-lane scratch dir (created, emptied of stale runs) ---------
function freshdir(name) {
  const d = DIR + "_" + name;
  io.mkdir(d);
  for (const f of io.readdir(d)) io.unlink(d + "/" + f);
  return d;
}

// ==========================================================================
//  u64 lane (spot trigram shape): scalar BigInt keys.
// ==========================================================================
{
  const dir = freshdir("u64");
  const idx = abc.index("u64", { dir, ext: ".u64", mem: 4096 });
  // brute-force oracle: a JS Set of the same scalars (newest-wins is moot for
  // a set; dedup is intrinsic).
  const oracle = new Set();
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };

  // batch 1 -> flush to run 0
  for (const v of [5, 100, 17, 42, 9, 256, 257, 258, 1000, 999]) add(v);
  idx.flush();
  eq(idx.runs.length, 1, "u64 one run after first flush");

  // batch 2 (overlaps) -> flush to run 1
  for (const v of [42, 7, 8, 9, 300, 301, 302, 2000, 256]) add(v);
  idx.flush();
  eq(idx.runs.length, 2, "u64 two runs after second flush");

  // batch 3 -> run 2 (tiny, drives the ladder)
  for (const v of [11, 12]) add(v);
  idx.flush();
  idx.compact();                        // 1/8 ladder: collapse the small tail
  if (idx.runs.length > 3) fail("u64 compact did not shrink the stack");

  // POINT: get every member + a non-member
  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const v of sorted) {
    const g = idx.get(v);
    eq(g, v, "u64 point hit " + v);
  }
  eq(idx.get(123456n), undefined, "u64 point miss");

  // RANGE [lo, hi): drain via the in-frame callback
  const collect = (lo, hi) => {
    const out = [];
    idx.range(BigInt(lo), BigInt(hi), (v) => { out.push(v); });   // undefined => continue
    return out;
  };
  const oracleRange = (lo, hi) =>
    sorted.filter((v) => v >= BigInt(lo) && v < BigInt(hi));
  eq(JSON.stringify(collect(8, 300).map(String)),
     JSON.stringify(oracleRange(8, 300).map(String)), "u64 range [8,300)");
  eq(JSON.stringify(collect(0, 100000).map(String)),
     JSON.stringify(oracleRange(0, 100000).map(String)), "u64 range all");

  // RANGE early stop: cb returns false after 3 hits
  {
    let n = 0;
    idx.range(0n, 100000n, () => { n++; return n < 3; });   // false => stop
    eq(n, 3, "u64 range early stop");
  }

  // PREFIX(p, lowBits) == range [p, p + 2^lowBits): the 256..258/300..302
  // block lives in [256, 256+256) and [256,512) but not in [256,256+2)
  const prefixCollect = (p, bits) => {
    const out = [];
    idx.prefix(BigInt(p), bits, (v) => { out.push(v); });
    return out;
  };
  eq(JSON.stringify(prefixCollect(256, 8).map(String)),
     JSON.stringify(oracleRange(256, 256 + 256).map(String)), "u64 prefix 256/8");

  io.log("index.js u64 OK");
}

// ==========================================================================
//  wh128 lane (keeper puppy registry shape): (key,val), point lookup on KEY.
//  Newest run shadows older at QUERY time (v1 deletes via shadowing, no
//  tombstone drop); compaction's full-element dedup collapses identical rows.
// ==========================================================================

//  --- part A: newest-run-wins SHADOW across runs (no compaction). ----------
{
  const dir = freshdir("wh128_shadow");
  const idx = abc.index("wh128", { dir, ext: ".w", mem: 4096 });
  // oracle Map key->val, newest write wins (mirrors the across-run shadow).
  const oracle = new Map();
  const add = (k, v) => { idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v)); };

  for (const [k, v] of [[10, 1], [20, 2], [30, 3], [40, 4], [50, 5]]) add(k, v);
  idx.flush();
  // batch 2 shadows key 30 with a NEW value in a NEWER run (newest-run-wins).
  for (const [k, v] of [[30, 99], [15, 15], [25, 25], [60, 6]]) add(k, v);
  idx.flush();
  eq(idx.runs.length, 2, "wh128 shadow: two runs");

  const keys = [...oracle.keys()].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const k of keys) eq(idx.get(k), oracle.get(k), "wh128 shadow point key " + k);
  eq(idx.get(30n), 99n, "wh128 newest-run-wins shadow");
  eq(idx.get(999n), undefined, "wh128 shadow point miss");
  io.log("index.js wh128 shadow OK");
}

//  --- part B: distinct (key,val) rows -> POINT/RANGE/PREFIX through compact.
//  Keys are unique per the keeper shape (a key maps to one stable val), so
//  full-element dedup is the only collapse and queries stay exact post-ladder.
{
  const dir = freshdir("wh128");
  const idx = abc.index("wh128", { dir, ext: ".w", mem: 4096 });
  const oracle = new Map();   // key -> val (unique keys)
  const add = (k, v) => { idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v)); };

  for (const [k, v] of [[10, 1], [20, 2], [30, 3], [40, 4], [50, 5]]) add(k, v);
  idx.flush();
  // batch 2: a DUPLICATE row (40,4) (collapses under dedup) + new keys.
  for (const [k, v] of [[40, 4], [15, 15], [25, 25], [60, 6], [70, 7]]) {
    idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v));
  }
  idx.flush();
  for (const [k, v] of [[5, 50], [80, 8]]) add(k, v);
  idx.flush();
  idx.compact();                            // 1/8 ladder collapses the tail
  if (idx.runs.length > 3) fail("wh128 compact did not shrink the stack");

  // POINT on key
  const keys = [...oracle.keys()].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const k of keys) eq(idx.get(k), oracle.get(k), "wh128 point key " + k);
  eq(idx.get(999n), undefined, "wh128 point miss");

  // RANGE over KEYS [lo, hi): cb([k,v]) per pair, ascending by (key,val).
  const collectKeys = (lo, hi) => {
    const out = [];
    idx.range(BigInt(lo), BigInt(hi), (pair) => { out.push(Number(pair[0])); });
    return out;
  };
  const oracleKeys = (lo, hi) =>
    keys.filter((k) => k >= BigInt(lo) && k < BigInt(hi)).map(Number);
  eq(JSON.stringify(collectKeys(10, 60)),
     JSON.stringify(oracleKeys(10, 60)), "wh128 range keys [10,60)");

  // RANGE early stop via cb returning false
  {
    let n = 0;
    idx.range(0n, 1000n, () => { n++; return n < 2; });
    eq(n, 2, "wh128 range early stop");
  }

  // PREFIX on key: [p, p + 2^bits)
  const prefixKeys = (p, bits) => {
    const out = [];
    idx.prefix(BigInt(p), bits, (pair) => { out.push(Number(pair[0])); });
    return out;
  };
  eq(JSON.stringify(prefixKeys(0, 6)),
     JSON.stringify(oracleKeys(0, 64)), "wh128 prefix 0/6");

  io.log("index.js wh128 OK");
}

// ==========================================================================
//  IN-MEMORY index (no opts.dir): io.ram runs, NO files on disk.  Same
//  put/flush/compact/get/range as on-disk; matches the oracle; leaves the
//  filesystem untouched (we watch a scratch dir for stray run files).
// ==========================================================================
{
  // a clean watch dir to prove NOTHING gets written to disk
  const watch = freshdir("mem_watch");
  const before = io.readdir(watch).sort().join(",");

  const idx = abc.index("u64", { mem: 4096 });    // no dir => in-memory
  eq(idx.onDisk, false, "in-mem onDisk flag");
  if (idx.dir != null) fail("in-mem index must have no dir");

  const oracle = new Set();
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };

  for (const v of [5, 100, 17, 42, 9, 256, 257, 258, 1000, 999]) add(v);
  idx.flush();
  eq(idx.runs.length, 1, "in-mem one run after first flush");
  if (idx.runs[0]._path != null) fail("in-mem run must have no _path (no file)");

  for (const v of [42, 7, 8, 9, 300, 301, 302, 2000, 256]) add(v);
  idx.flush();
  for (const v of [11, 12]) add(v);
  idx.flush();
  idx.compact();                                  // RAM->RAM merge
  if (idx.runs.length > 3) fail("in-mem compact did not shrink the stack");

  // POINT + RANGE + PREFIX vs the oracle (identical to the on-disk path)
  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const v of sorted) eq(idx.get(v), v, "in-mem point hit " + v);
  eq(idx.get(123456n), undefined, "in-mem point miss");

  const collect = (lo, hi) => {
    const out = [];
    idx.range(BigInt(lo), BigInt(hi), (v) => { out.push(v); });
    return out;
  };
  const oracleRange = (lo, hi) =>
    sorted.filter((v) => v >= BigInt(lo) && v < BigInt(hi));
  eq(JSON.stringify(collect(8, 300).map(String)),
     JSON.stringify(oracleRange(8, 300).map(String)), "in-mem range [8,300)");
  const pfx = [];
  idx.prefix(256n, 8, (v) => { pfx.push(v); });
  eq(JSON.stringify(pfx.map(String)),
     JSON.stringify(oracleRange(256, 256 + 256).map(String)), "in-mem prefix 256/8");

  // NO FILES: the watch dir is unchanged AND nothing was written under it.
  const after = io.readdir(watch).sort().join(",");
  eq(after, before, "in-mem index wrote NO files to disk");

  io.log("index.js in-memory OK");
}

// ==========================================================================
//  .seek(needle) PULL cursor: positions every source at the first entry
//  >= needle, then .next() advances ONE merged entry at a time in sorted
//  order; full-element newest-wins dedup; .key/.val/.entry; false at end.
//  Pulled across MULTIPLE runs + a NON-EMPTY memtable, for BOTH lanes; the
//  caller stops early after K and matches the oracle's tail slice.
// ==========================================================================

//  --- u64: scalars across 3 runs + live memtable ---------------------------
{
  const dir = freshdir("seek_u64");
  const idx = abc.index("u64", { dir, ext: ".u64", mem: 4096 });
  const oracle = new Set();
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };

  for (const v of [5, 100, 17, 42, 9, 256]) add(v);   idx.flush();
  for (const v of [42, 7, 8, 300, 256, 9]) add(v);    idx.flush();   // overlaps
  for (const v of [11, 12, 999]) add(v);              idx.flush();
  for (const v of [3, 13, 257, 9]) add(v);            // <- live memtable, no flush

  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const tailFrom = (needle) => sorted.filter((v) => v >= BigInt(needle));

  // full pull from a needle: must equal the oracle's tail slice exactly
  const pullAll = (needle) => {
    const out = [];
    const c = idx.seek(BigInt(needle));
    while (c.next()) out.push(c.val);
    return out;
  };
  eq(JSON.stringify(pullAll(0).map(String)),
     JSON.stringify(tailFrom(0).map(String)), "u64 seek(0) full");
  eq(JSON.stringify(pullAll(10).map(String)),
     JSON.stringify(tailFrom(10).map(String)), "u64 seek(10) tail");
  eq(JSON.stringify(pullAll(256).map(String)),
     JSON.stringify(tailFrom(256).map(String)), "u64 seek(256) tail");
  eq(JSON.stringify(pullAll(100000).map(String)),
     JSON.stringify(tailFrom(100000).map(String)), "u64 seek past-end empty");

  // dedup: every value distinct, none repeats (the heads overlap across runs)
  {
    const all = pullAll(0);
    const seen = new Set(all.map(String));
    eq(seen.size, all.length, "u64 seek dedups duplicates across runs+mem");
  }

  // .key/.val/.entry mirror for a scalar lane; cursor positions on first >=
  {
    const c = idx.seek(11n);
    if (!c.next()) fail("u64 seek(11) yields nothing");
    eq(c.key, 11n, "u64 seek first key");
    eq(c.val, 11n, "u64 seek first val");
    eq(c.entry, 11n, "u64 seek first entry");
  }

  // stop EARLY after K: pull 3 from needle=8, compare to the oracle tail head
  {
    const want = tailFrom(8).slice(0, 3);
    const got = [];
    const c = idx.seek(8n);
    for (let i = 0; i < 3 && c.next(); i++) got.push(c.val);
    eq(JSON.stringify(got.map(String)),
       JSON.stringify(want.map(String)), "u64 seek early-stop after 3");
  }

  io.log("index.js seek u64 OK");
}

//  --- wh128: (key,val) across 3 runs + live memtable -----------------------
{
  const dir = freshdir("seek_wh128");
  const idx = abc.index("wh128", { dir, ext: ".w", mem: 4096 });
  // unique keys (keeper shape): seek/range order by (key,val), dedup is
  // full-element.  Oracle = Map key->val, newest write wins (shadow).
  const oracle = new Map();
  const add = (k, v) => { idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v)); };

  for (const [k, v] of [[10, 1], [20, 2], [30, 3], [40, 4]]) add(k, v);  idx.flush();
  for (const [k, v] of [[40, 4], [15, 15], [25, 25], [60, 6]]) add(k, v); idx.flush(); // dup (40,4)
  for (const [k, v] of [[5, 50], [80, 8]]) add(k, v);                     idx.flush();
  for (const [k, v] of [[12, 12], [70, 7]]) add(k, v);                    // live memtable

  // pairs sorted by (key,val); with unique keys this is key order
  const pairs = [...oracle.entries()]
    .map(([k, v]) => [k, v])
    .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  const tailFrom = (k) => pairs.filter((p) => p[0] >= BigInt(k));

  const pullAll = (k) => {
    const out = [];
    const c = idx.seek(BigInt(k), 0n);
    while (c.next()) out.push([Number(c.key), Number(c.val)]);
    return out;
  };
  eq(JSON.stringify(pullAll(0)),
     JSON.stringify(tailFrom(0).map((p) => [Number(p[0]), Number(p[1])])),
     "wh128 seek(0) full");
  eq(JSON.stringify(pullAll(20)),
     JSON.stringify(tailFrom(20).map((p) => [Number(p[0]), Number(p[1])])),
     "wh128 seek(20) tail");
  eq(JSON.stringify(pullAll(1000)), "[]", "wh128 seek past-end empty");

  // .entry is the [key,val] pair; cursor positions on first key >= needle
  {
    const c = idx.seek(15n, 0n);
    if (!c.next()) fail("wh128 seek(15) yields nothing");
    eq(c.key, 15n, "wh128 seek first key");
    eq(c.val, 15n, "wh128 seek first val");
    eq(JSON.stringify([Number(c.entry[0]), Number(c.entry[1])]),
       JSON.stringify([15, 15]), "wh128 seek first entry pair");
  }

  // dedup: the duplicated (40,4) row appears ONCE
  {
    const all = pullAll(0);
    const n40 = all.filter((p) => p[0] === 40).length;
    eq(n40, 1, "wh128 seek collapses the duplicated (40,4) row");
  }

  // stop EARLY after K=2 from key>=20
  {
    const want = tailFrom(20).slice(0, 2).map((p) => [Number(p[0]), Number(p[1])]);
    const got = [];
    const c = idx.seek(20n, 0n);
    for (let i = 0; i < 2 && c.next(); i++) got.push([Number(c.key), Number(c.val)]);
    eq(JSON.stringify(got), JSON.stringify(want), "wh128 seek early-stop after 2");
  }

  io.log("index.js seek wh128 OK");
}

// ==========================================================================
//  JS-105: a warm (unchanged) memtable must NOT be re-sorted per read — sort
//  once when dirty (after a put), then get/range/prefix/seek skip it.  We
//  count mem.sort calls by shadowing .sort on the instance, and interleave
//  put/get/range against the oracle to prove reads stay exact throughout.
// ==========================================================================

//  --- u64: interleaved put/get/range correctness + sort-call counting ------
{
  const idx = abc.index("u64", { mem: 4096 });        // in-memory
  const oracle = new Set();
  let sorts = 0;
  const orig = idx.mem.sort;
  idx.mem.sort = function () { sorts++; return orig.call(this); };
  if (idx.mem.sort === orig) fail("JS-105 sort shadow did not take");
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };

  // interleave: after EVERY put, a hit and a miss must stay exact
  for (const v of [42, 7, 999, 8, 300, 5, 100, 17, 256, 12, 2000, 11]) {
    add(v);
    eq(idx.get(BigInt(v)), BigInt(v), "JS-105 u64 hit after put " + v);
    eq(idx.get(777777n), undefined, "JS-105 u64 miss after put " + v);
  }
  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const out = [];
  idx.range(0n, 100000n, (v) => { out.push(v); });
  eq(JSON.stringify(out.map(String)),
     JSON.stringify(sorted.map(String)), "JS-105 u64 range all");

  // WARM reads: the memtable is sorted+clean now; zero further sorts
  const warm = sorts;
  for (let i = 0; i < 50; i++) eq(idx.get(42n), 42n, "JS-105 u64 warm get");
  idx.range(5n, 500n, () => {});
  idx.prefix(256n, 8, () => {});
  const c = idx.seek(0n);
  while (c.next());
  eq(sorts, warm, "JS-105 u64 warm reads must not re-sort the memtable");

  // one put dirties -> exactly ONE sort over the next read burst
  add(4444);
  for (let i = 0; i < 10; i++) eq(idx.get(4444n), 4444n, "JS-105 u64 get 4444");
  eq(sorts, warm + 1, "JS-105 u64 one sort after a put, then warm again");

  // flush empties the memtable; reads hit runs only and stay exact
  idx.flush();
  for (const v of [...oracle]) eq(idx.get(v), v, "JS-105 u64 post-flush hit " + v);
  const postFlush = sorts;
  for (let i = 0; i < 20; i++) idx.get(11n);
  eq(sorts, postFlush, "JS-105 u64 post-flush reads never sort");

  io.log("index.js JS-105 u64 OK");
}

//  --- wh128: interleaved put/get/range stays exact; warm reads skip sort ---
{
  const dir = freshdir("js105_wh128");
  const idx = abc.index("wh128", { dir, ext: ".w", mem: 4096 });
  const oracle = new Map();                           // unique keys per batch
  let sorts = 0;
  const orig = idx.mem.sort;
  idx.mem.sort = function () { sorts++; return orig.call(this); };
  const add = (k, v) => { idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v)); };

  for (const [k, v] of [[10, 1], [20, 2], [30, 3], [40, 4]]) {
    add(k, v);
    eq(idx.get(BigInt(k)), BigInt(v), "JS-105 wh128 hit after put " + k);
    eq(idx.get(999n), undefined, "JS-105 wh128 miss after put " + k);
  }
  idx.flush();                                        // batch 1 -> run 0
  // batch 2 shadows key 30 via a NEWER memtable entry; interleave reads
  for (const [k, v] of [[30, 99], [15, 15], [60, 6]]) {
    add(k, v);
    eq(idx.get(BigInt(k)), BigInt(v), "JS-105 wh128 hit after put " + k);
  }
  eq(idx.get(30n), 99n, "JS-105 wh128 memtable shadows the run");

  // range has NO cross-run key dedup (v1): BOTH rows for key 30 stream out,
  // in (key,val) order — [10,1] [15,15] [20,2] [30,3] [30,99] [40,4] [60,6].
  const got = [];
  idx.range(0n, 1000n, (pair) => { got.push(Number(pair[0])); });
  eq(JSON.stringify(got), JSON.stringify([10, 15, 20, 30, 30, 40, 60]),
     "JS-105 wh128 range keys");

  // WARM: repeated point/range reads on the unchanged memtable never sort
  const warm = sorts;
  const keys = [...oracle.keys()].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const k of keys) eq(idx.get(k), oracle.get(k), "JS-105 wh128 warm key " + k);
  idx.range(0n, 1000n, () => {});
  eq(sorts, warm, "JS-105 wh128 warm reads must not re-sort the memtable");

  io.log("index.js JS-105 wh128 OK");
}

io.log("index.js OK");
