"use strict";
// DOG-027: abc.index(lane, {dir, ext}) — a HANDLE on a dog Pup stack.  The LSM
// (runs + memtable + the 1/8 ladder + the run naming) is C; this surface is
// put / commit / get / range / seek / count / run / drop / close and nothing
// else — `runs`, `_seq`, `mem`, `flush` and `compact` are gone.
//
// Three lanes: kv64 (keyed — newest wins per key), wh128 (keeper puppy
// registry: (key,val) rows, point on KEY) and u64 (spot trigram: scalar).
// Queries stream through an IN-FRAME callback whose return is a stop signal.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }
function throws(f) { try { f(); } catch (x) { return "" + x; } return ""; }

const DIR = "/tmp/jabc_index_" + process.pid;

// --- a clean per-lane scratch dir (created, emptied of stale runs) ---------
function freshdir(name) {
  const d = DIR + "_" + name;
  io.mkdir(d);
  for (const f of io.readdir(d)) io.unlink(d + "/" + f);
  return d;
}

// ==========================================================================
//  put / get / commit round trip, per lane.  A put lands in the memtable and
//  is visible at once; commit seals it into a run and the answers do not move.
// ==========================================================================
{
  const dir = freshdir("u64");
  const idx = abc.index("u64", { dir, ext: ".u64" });
  const oracle = new Set();
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };

  for (const v of [5, 100, 17, 42, 9, 256, 257, 258, 1000, 999]) add(v);
  eq(idx.get(42n), 42n, "u64 memtable hit before commit");
  idx.commit();
  for (const v of [42, 7, 8, 9, 300, 301, 302, 2000, 256]) add(v);
  idx.commit();
  for (const v of [11, 12]) add(v);          // live memtable, uncommitted

  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  for (const v of sorted) eq(idx.get(v), v, "u64 point hit " + v);
  eq(idx.get(123456n), undefined, "u64 point miss");

  // RANGE [lo, hi) through the in-frame cb (undefined => keep going)
  const collect = (lo, hi) => {
    const out = [];
    idx.range(BigInt(lo), BigInt(hi), (v) => { out.push(v); });
    return out;
  };
  const oracleRange = (lo, hi) =>
    sorted.filter((v) => v >= BigInt(lo) && v < BigInt(hi));
  eq(JSON.stringify(collect(8, 300).map(String)),
     JSON.stringify(oracleRange(8, 300).map(String)), "u64 range [8,300)");
  eq(JSON.stringify(collect(0, 100000).map(String)),
     JSON.stringify(oracleRange(0, 100000).map(String)), "u64 range all");

  // early stop: `false` and "enough" both end the scan
  {
    let n = 0;
    idx.range(0n, 100000n, () => { n++; return n < 3; });
    eq(n, 3, "u64 range early stop on false");
    let m = 0;
    idx.range(0n, 100000n, () => { m++; return m >= 4 ? "enough" : "more"; });
    eq(m, 4, "u64 range early stop on enough");
  }

  // prefix(p, bits) == range [p, p + 2^bits)
  {
    const out = [];
    idx.prefix(256n, 8, (v) => { out.push(v); });
    eq(JSON.stringify(out.map(String)),
       JSON.stringify(oracleRange(256, 256 + 256).map(String)), "u64 prefix 256/8");
  }
  idx.close();
  io.log("index.js u64 OK");
}

// ==========================================================================
//  wh128: (key,val) rows, point lookup on the KEY.  Rows with one key and
//  different vals coexist (the lane's Z is the whole element); an identical
//  row across runs collapses.
// ==========================================================================
{
  const dir = freshdir("wh128");
  const idx = abc.index("wh128", { dir, ext: ".w" });
  const oracle = new Map();
  const add = (k, v) => { idx.put(BigInt(k), BigInt(v)); oracle.set(BigInt(k), BigInt(v)); };

  for (const [k, v] of [[10, 1], [20, 2], [30, 3], [40, 4]]) add(k, v);
  idx.commit();
  for (const [k, v] of [[40, 4], [15, 15], [25, 25], [60, 6]]) add(k, v);  // dup row (40,4)
  idx.commit();
  for (const [k, v] of [[12, 12], [70, 7]]) add(k, v);                     // live memtable

  for (const [k, v] of oracle) eq(idx.get(k), v, "wh128 point hit " + k);
  eq(idx.get(999n), undefined, "wh128 point miss");

  // the duplicated (40,4) row is emitted ONCE
  {
    const out = [];
    idx.range(0n, 1000n, (p) => { out.push(Number(p[0])); });
    eq(out.filter((k) => k === 40).length, 1, "wh128 dup row collapses");
    eq(JSON.stringify(out), JSON.stringify([...oracle.keys()].map(Number).sort((a, b) => a - b)),
       "wh128 range == oracle keys");
  }
  idx.close();
  io.log("index.js wh128 OK");
}

// ==========================================================================
//  kv64 is the KEYED lane: kv64Z compares keys only, so a second put of one
//  key SHADOWS the first — newest wins, memtable over run and run over run.
// ==========================================================================
{
  const dir = freshdir("kv64");
  const idx = abc.index("kv64", { dir, ext: ".kv" });
  idx.put(10, 1).put(20, 2).put(30, 3).commit();     // run 0
  idx.put(20, 22).commit();                          // run 1 shadows key 20
  idx.put(30, 33);                                   // memtable shadows key 30

  eq(idx.get(10), 1n, "kv64 untouched key");
  eq(idx.get(20), 22n, "kv64 newer RUN wins");
  eq(idx.get(30), 33n, "kv64 MEMTABLE wins over the run");
  eq(idx.get(99), undefined, "kv64 point miss");

  // a merged scan yields one row per key, each the newest
  {
    const out = [];
    idx.range(0, 1000, (p) => { out.push(Number(p[0]) + "=" + Number(p[1])); });
    eq(out.join(","), "10=1,20=22,30=33", "kv64 range newest-wins");
  }
  // and the same through the seek cursor
  {
    const c = idx.seek(0);
    const out = [];
    while (c.next()) out.push(Number(c.key) + "=" + Number(c.val));
    eq(out.join(","), "10=1,20=22,30=33", "kv64 seek newest-wins");
  }
  idx.close();
  io.log("index.js kv64 newest-wins OK");
}

// ==========================================================================
//  seek(k): a merged PULL cursor at the first row >= k, ascending, deduped,
//  spanning the committed runs AND the live memtable.  .key/.val/.entry, and
//  next() goes false past the last row.
// ==========================================================================
{
  const dir = freshdir("seek");
  const idx = abc.index("u64", { dir, ext: ".u64" });
  const oracle = new Set();
  const add = (v) => { idx.put(BigInt(v)); oracle.add(BigInt(v)); };
  for (const v of [5, 100, 17, 42, 9]) add(v);   idx.commit();
  for (const v of [7, 8, 9, 300, 256]) add(v);   idx.commit();
  for (const v of [11, 12]) add(v);              idx.commit();
  for (const v of [3, 500]) add(v);              // live memtable
  const sorted = [...oracle].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const tailFrom = (k) => sorted.filter((v) => v >= BigInt(k));

  const pullAll = (k) => {
    const out = [];
    const c = idx.seek(BigInt(k));
    while (c.next()) out.push(c.val);
    return out;
  };
  eq(JSON.stringify(pullAll(0).map(String)),
     JSON.stringify(tailFrom(0).map(String)), "seek(0) full merge");
  eq(JSON.stringify(pullAll(10).map(String)),
     JSON.stringify(tailFrom(10).map(String)), "seek(10) tail");
  eq(JSON.stringify(pullAll(100000)), "[]", "seek past the end is empty");

  {
    const c = idx.seek(10n);
    if (!c.next()) fail("seek(10) yields nothing");
    eq(c.key, 11n, "seek first key");
    eq(c.val, 11n, "seek first val");
    eq(c.entry, 11n, "seek first entry");
    // stop EARLY: pull 2 more and walk away
    const got = [];
    for (let i = 0; i < 2 && c.next(); i++) got.push(c.val);
    eq(JSON.stringify(got.map(String)),
       JSON.stringify(tailFrom(10).slice(1, 3).map(String)), "seek early stop");
  }
  idx.close();
  io.log("index.js seek OK");
}

// ==========================================================================
//  run(i) / drop(i): the marker-audit path.  A family reads run i, finds no
//  marker row, and unlinks it — the file goes and the queries forget it.
// ==========================================================================
{
  const dir = freshdir("audit");
  const idx = abc.index("kv64", { dir, ext: ".kv" });
  //  each run under a 1/8 of the one before it, so the ladder leaves the
  //  stack alone and there are three runs to audit
  for (let i = 0; i < 100; i++) idx.put(1000 + i, i);
  idx.commit();
  for (let i = 0; i < 10; i++) idx.put(2000 + i, i);
  idx.commit();
  idx.put(3000, 3).commit();
  eq(idx.count, 3, "audit three committed runs");
  eq(io.readdir(dir).length, 3, "audit three files on disk");

  // run(i) is a read-only lane-typed view, oldest-first
  eq(idx.run(0).length, 200, "run(0) is 100 kv64 rows = 200 BigUint64 cells");
  eq(idx.run(0)[0], 1000n, "run(0) first key");
  eq(idx.run(1)[0], 2000n, "run(1) first key — oldest-first");
  eq(idx.run(2)[0], 3000n, "run(2) first key");
  if (!throws(() => idx.run(9))) fail("run(9) must throw");

  // drop the MIDDLE run: gone from disk and from every query
  idx.drop(1);
  eq(idx.count, 2, "after drop(1) two runs");
  eq(io.readdir(dir).length, 2, "after drop(1) two files");
  eq(idx.get(2005), undefined, "the dropped run's keys are gone");
  eq(idx.get(1005), 5n, "the surviving older run answers");
  eq(idx.get(3000), 3n, "the surviving younger run answers");
  eq(idx.run(1)[0], 3000n, "the stack closed the gap");

  // drop() with no argument drops them all — the re-derive case
  idx.drop();
  eq(idx.count, 0, "drop() drops every run");
  eq(io.readdir(dir).length, 0, "drop() emptied the dir");
  eq(idx.get(1005), undefined, "nothing answers after drop()");
  idx.close();
  io.log("index.js run/drop audit OK");
}

// ==========================================================================
//  Reopening: a dir keeps its ron64-named runs across handles.  A run left by
//  the RETIRED JS writer (8-char zero-padded name) is NOT a run to the 10-char
//  scan, so opening the dir IGNORES it — the index reads empty and the family
//  recomputes it, but the legacy file stays on disk; an open deletes nothing.
// ==========================================================================
{
  const dir = freshdir("reopen");
  // two legacy runs, written the way jab's JS memtable used to book them
  for (const seq of [1, 2]) {
    const mem = abc.ram("HEAPu64", 128);
    for (let i = 0; i < 100; i++) mem.push(BigInt(100 * seq + i));
    mem.sort();
    const out = abc.book("HEAPu64", dir + "/" + String(seq).padStart(8, "0") + ".u64", mem.size);
    abc.merge([mem], out);
    abc.close(out);
  }
  eq(io.readdir(dir).length, 2, "two legacy files planted");
  {
    const idx = abc.index("u64", { dir, ext: ".u64" });
    eq(io.readdir(dir).length, 2, "opening LEFT the legacy files alone");
    eq(idx.count, 0, "the index reads empty");
    eq(idx.get(117n), undefined, "a legacy row does not answer");
    // and the handle is perfectly usable: the family just recomputes
    for (let i = 0; i < 100; i++) idx.put(BigInt(100 + i));
    idx.commit();
    eq(idx.count, 1, "the recomputed run committed");
    eq(idx.get(117n), 117n, "the recomputed index answers");
    idx.close();
  }
  // reopen: the ron64-named run comes back, and nothing else was harmed
  {
    const idx = abc.index("u64", { dir, ext: ".u64" });
    eq(idx.count, 1, "the ron64 run reopened");
    eq(idx.get(140n), 140n, "and answers after the reopen");
    eq(io.readdir(dir).length, 3, "the ron64 run + both legacy files survive");
    idx.close();
  }
  io.log("index.js reopen + legacy skip OK");
}

// ==========================================================================
//  The ladder is C's business: a page-full memtable seals itself and the
//  stack stays shallow.  JS never sees a run count it did not commit for.
// ==========================================================================
{
  const dir = freshdir("ladder");
  const idx = abc.index("u64", { dir, ext: ".u64" });
  for (let i = 0; i < 3000; i++) idx.put(BigInt(100000 - i));
  idx.commit();
  if (idx.count > 4) fail("the ladder let the stack grow to " + idx.count);
  eq(idx.get(100000n - 2999n), 100000n - 2999n, "the oldest row survived");
  eq(idx.get(100000n), 100000n, "the newest row survived");
  {
    let n = 0;
    idx.range(0n, 1000000n, () => { n++; });
    eq(n, 3000, "every row is queryable after the ladder ran");
  }
  idx.close();
  io.log("index.js ladder OK");
}

// ==========================================================================
//  DOG-027: ONE run cap (HIT_MAX_RUNS = 64).  Above it every entry point just
//  THROWS, in plain words — no windowing, no repair cascade.  A stack that
//  deep is damaged: drop the runs and re-derive.
// ==========================================================================
{
  const CAP = 64;
  const mkruns = (n) => {
    const rs = [];
    for (let i = 0; i < n; i++) {
      const h = abc.ram("HEAPu64", 1);
      h.push(BigInt(i));
      rs.push(h);
    }
    return rs;
  };
  const over = mkruns(CAP + 1), at = mkruns(CAP);

  for (const [name, f] of [
    ["merge",     (rs) => abc.merge(rs)],
    ["intersect", (rs) => abc.intersect(rs)],
    ["compact",   (rs) => abc._compact_u64(rs, abc.ram("HEAPu64", 128))],
    ["seekrange", (rs) => abc._seekrange_u64(rs, 0n, 1000n, () => {})],
  ]) {
    const msg = throws(() => f(over));
    if (!msg.includes("too many index runs"))
      fail("DOG-027 " + name + " above the cap: " + (msg || "no throw"));
    const ok = throws(() => f(at));
    if (ok) fail("DOG-027 " + name + " AT the cap must work: " + ok);
  }
  eq(abc.merge(mkruns(CAP)).length, CAP, "DOG-027 merge at the cap");

  // and a DIRECTORY deeper than the cap: the index refuses to open it, in
  // plain words.  Such a stack is foreign — the cure is to re-derive it.
  // The names are real 10-char RON64 pup keys, zero-padded exactly the way
  // dog writes them — an 8-wide name would be unlinked as a legacy run.
  {
    const dir = freshdir("cap");
    const name10 = (k) => {
      const s = ron.encode(BigInt(k));
      return "0".repeat(10 - s.length) + s;
    };
    eq(name10(1000).length, 10, "a padded pup key is 10 RON64 chars");
    //  RON64 is CASE-SENSITIVE ('A' is 10, 'a' is 37) and macOS/APFS is not,
    //  so keys 27 apart in one digit name the SAME file there.  Pick keys whose
    //  two low RON64 digits are both DECIMAL — 10*a + b, a<7, b<10 — so the
    //  CAP+2 planted names stay distinct on a case-folding filesystem too.
    const key = (i) => (((i / 10) | 0) << 6) | (i % 10);
    for (let i = 0; i < CAP + 2; i++) {
      const mem = abc.ram("HEAPu64", 1);
      mem.push(BigInt(i));
      const out = abc.book("HEAPu64", dir + "/" + name10(key(i)) + ".u64", 1);
      abc.merge([mem], out);
      abc.close(out);
    }
    eq(io.readdir(dir).length, CAP + 2, "the over-deep dir is planted");
    const msg = throws(() => abc.index("u64", { dir, ext: ".u64" }));
    if (!msg.includes("too many index runs"))
      fail("DOG-027 an over-deep dir must not open: " + (msg || "no throw"));
  }
  io.log("index.js DOG-027 run cap OK");
}

io.log("index.js OK");
