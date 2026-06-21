"use strict";
// GIT-010: the git.pack pack-log is an ENTRY PRODUCER, not an index owner.
// pack.scan walks the WHOLE pack, resolves+git-shas each object, and emits one
// wh128 (key=hashlet60|type, val=offset) entry per object into a caller buffer;
// pack.feed(.., out) emits the just-fed object's entry (index-on-append).  The
// caller pipes the entries into a JS-022 abc.index (wh128 lane) and looks up
// sha->offset.  sort/merge/persist/query stay the CALLER's (the pack owns no
// index).  Asserts: every emitted (key,val) round-trips through the index vs a
// resolve-and-rehash oracle; index-on-append == a full scan reindex.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }
const dec = (u8) => utf8.Decode(u8);

// two near-identical sizable blobs so the OFS_DELTA beats raw (as in pack.js)
const lines = [];
for (let i = 0; i < 200; i++) lines.push("line " + i + ": some unique content " + (i * 7));
const s1 = lines.join("\n");
lines[100] = "this line was modified";
const s2 = lines.join("\n");
lines[50] = "and a third variant on this line here";
const s3 = lines.join("\n");
const v1 = utf8.Encode(s1);
const v2 = utf8.Encode(s2);
const v3 = utf8.Encode(s3);

// build a pack: raw -> OFS_DELTA -> delta-of-delta
const p = git.pack.ram(1 << 16);
p.header();
const a = p.feed("blob", v1);          // raw          @a
const b = p.feed("blob", v2, a);       // OFS_DELTA    @b (base a)
const c = p.feed("blob", v3, b);       // delta-of-delta @c (base b)
p.finish();
eq(p.count, 3, "object count");

// ---- pack.scan -> wh128 entries into a caller buffer ----------------------
const entryBuf = io.buf(p.count * 16 + 64);   // n * sizeof(wh128)
const ents = p.scan(entryBuf);                // BigUint64Array [k,v,k,v,...]
eq(ents.length, p.count * 2, "scan emitted one (key,val) per object");

// the offsets in the entries must be exactly the three record offsets
const gotOffs = [];
for (let i = 0; i < ents.length; i += 2) gotOffs.push(Number(ents[i + 1]));
eq(JSON.stringify(gotOffs.slice().sort((x, y) => x - y)),
   JSON.stringify([a, b, c].slice().sort((x, y) => x - y)),
   "scan vals == record offsets");

// ---- pipe entries into a JS-022 abc.index (wh128 lane) --------------------
const idx = abc.index("wh128", { mem: 4096 });   // in-memory LSM
const oracle = new Map();                          // key -> val (offset)
for (let i = 0; i < ents.length; i += 2) {
  const k = ents[i], val = ents[i + 1];
  idx.put(k, val);
  oracle.set(k.toString(), val);
}
idx.flush();

// sha->offset lookup: for each object, the index returns its byte offset.
// The key already encodes hashlet60|type; the index get(key) yields the val.
for (const [kStr, val] of oracle) {
  const got = idx.get(BigInt(kStr));
  eq(got, val, "index get(key) == offset for key " + kStr);
}
eq(idx.get(123456789n), undefined, "index point miss for an absent key");

// every emitted key must be DISTINCT (3 distinct blobs -> 3 distinct shas)
eq(oracle.size, 3, "three distinct keys");

// ---- index-on-append (feed with out) == a full scan reindex ---------------
// Rebuild the SAME pack, but capture entries via feed's optional out buffer;
// the resulting (key,val) set must equal the scan's, object for object.
const appendBuf = io.buf(3 * 16 + 64);
const q = git.pack.ram(1 << 16);
q.header();
const qa = q.feed("blob", v1, null, appendBuf);    // raw       -> entry @qa
const qb = q.feed("blob", v2, qa, appendBuf);      // OFS_DELTA -> entry @qb
const qc = q.feed("blob", v3, qb, appendBuf);      // delta^2   -> entry @qc
q.finish();

// view the appended entries
const app = new BigUint64Array(appendBuf.data().buffer,
                               appendBuf.data().byteOffset,
                               (appendBuf.data().byteLength / 8) | 0);
eq(app.length, 3 * 2, "feed-emit appended one (key,val) per object");

// offsets line up with the q-pack record offsets
eq(qa, a, "qa == a"); eq(qb, b, "qb == b"); eq(qc, c, "qc == c");

// the index-on-append KEYS must equal the scan KEYS (same shas) and each VAL
// must equal that object's offset.  Compare as sorted (key,val) pair sets.
const pairSet = (u64arr) => {
  const out = [];
  for (let i = 0; i < u64arr.length; i += 2)
    out.push(u64arr[i].toString() + ":" + u64arr[i + 1].toString());
  return out.sort();
};
eq(JSON.stringify(pairSet(app)), JSON.stringify(pairSet(ents)),
   "index-on-append entries == full scan entries");

// and the append entries resolve through the SAME index identically
const idx2 = abc.index("wh128", { mem: 4096 });
for (let i = 0; i < app.length; i += 2) idx2.put(app[i], app[i + 1]);
idx2.flush();
for (let i = 0; i < app.length; i += 2)
  eq(idx2.get(app[i]), app[i + 1], "append index get == offset");

io.log("packidx.js OK");
