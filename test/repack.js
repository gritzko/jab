"use strict";
//  JAB-020: git.pack(fd, buf, shard, opts) — the ONE-call stream ingest.
//  Hermetic: the source pack is built here with the existing pack writer (no
//  git, no network), fed to the binding through a real fd, and the produced
//  `<shard>/NNNNNNNNNN.keeper` logs are read back with the native scanner.
//  Asserts: every object survives with its identity (the index entries the
//  binding wrote into the CALLER's region are exactly the scanner's keys),
//  the Buf's cursor comes back advanced, and a small cap rotates logs while
//  keeping the object count whole.
function fail(m) { throw "FAIL " + m; }

const DIR = "/tmp/jabc_repack_" + io.getpid();
const SRC = DIR + "/src.pack";
try { io.mkdir(DIR); } catch (e) {}

//  --- a source pack: N blobs, the last one a delta against its neighbour ---
const N = 12;
const p = git.pack.ram(1 << 16).header();
const bodies = [];
for (let i = 0; i < N - 1; i++) {
  const body = utf8.Encode("repack test object " + i + "\n" + "x".repeat(i * 7));
  bodies.push(body);
  p.feed("blob", body);
}
//  one OFS_DELTA: same shape as its base, so the writer picks a delta
const prev = p.buffer.watermark;
const near = utf8.Encode("repack test object 9\n" + "x".repeat(63) + "!");
p.feed("blob", near, 12);            //  base = the FIRST record (offset 12)
bodies.push(near);
abc._pack_header(p, 0, N);           //  patch the header's object count

const wfd = io.open(SRC, "c");
io._write(wfd, p.subarray(0, p.buffer.watermark));
io.close(wfd);

//  --- one call: the whole pack into one log ---------------------------------
function run(shard, cap) {
  try { io.mkdir(shard); } catch (e) {}
  const fd = io.open(SRC, "r");
  const buf = io.buf(1 << 20);
  const idx = abc.ram("HEAPwh128", 1024);
  let seen = 0;
  const st = git.pack(fd, buf, shard, {
    cap: cap || 0, log0: 0, index: idx, every: 1,
    onStep: (s) => { seen = s.objects; },
  });
  io.close(fd);
  return { st: st, idx: idx, buf: buf, seen: seen };
}

const one = run(DIR + "/one", 0);
if (one.st.objects !== N) fail("objects " + one.st.objects + " != " + N);
if (one.st.total !== N) fail("header count " + one.st.total + " != " + N);
if (one.st.logs !== 1) fail("logs " + one.st.logs + " != 1");
if (one.st.indexN !== N + 1) fail("index " + one.st.indexN + " != " + (N + 1));
if (one.st.ofs < 1) fail("no delta survived as OFS (" + one.st.ofs + ")");
if (one.seen !== N) fail("progress never reached the end (" + one.seen + ")");
//  The cursor comes back ADVANCED: everything the loop ate is PAST, and what
//  it did not eat stays DATA (over a git-written pack that is the 20-byte
//  trailer; this synthetic source carries none, so DATA lands empty).
if (one.buf._idle !== one.buf._data)
  fail("buf cursor: " + one.buf._data + ".." + one.buf._idle + " (want empty)");
if (one.buf._data === 0) fail("buf cursor never advanced");

//  --- the index the binding wrote == what the scanner reads back ------------
const keysOf = (r) => {
  const out = [];
  for (let i = 0; i < r.st.indexN; i++) {
    const key = r.idx[i * 2], val = r.idx[i * 2 + 1];
    if ((key & 0xfn) === 0xfn) continue;              //  the PACK summary
    out.push(key.toString(16) + "@" + ((val >> 24n) & 0xffffffffffn));
  }
  return out.sort();
};
const scanned = (path) => {
  const pk = git.pack.mmap(path, "r");
  pk.buffer.watermark = pk.byteLength;
  const e = pk.scan(io.buf((pk.count || 0) * 16 + 256));
  const out = [];
  for (let i = 0; i < e.length; i += 2)
    out.push(e[i].toString(16) + "@" + (e[i + 1] & 0xffffffffffn));
  return out.sort();
};
const mine = keysOf(one).join(",");
const theirs = scanned(DIR + "/one/0000000000.keeper").join(",");
if (mine !== theirs) fail("index != scan\n  " + mine + "\n  " + theirs);
if (keysOf(one).length !== N) fail("index entry count " + keysOf(one).length);

//  --- a small cap rotates logs; nothing is lost ----------------------------
const many = run(DIR + "/many", 200);
if (many.st.objects !== N) fail("rotated objects " + many.st.objects);
if (many.st.logs < 2) fail("cap 200 did not rotate (" + many.st.logs + " logs)");
if (many.st.indexN !== N + many.st.logs)
  fail("rotated index " + many.st.indexN + " for " + many.st.logs + " logs");
//  a delta whose base landed in an earlier log is re-anchored by sha (KEEP-006)
if (many.st.ref + many.st.ofs < 1) fail("the delta vanished across the split");
