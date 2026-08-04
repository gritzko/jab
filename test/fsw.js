"use strict";
// JAB-031: fsw — a watcher fd, an armed dir, a created file, a drained name.
// inotify hands back a BARE basename; kqueue hands back nothing (empty name,
// a rescan signal), so a name assert only fires when a name was reported.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const dir = "/tmp/jab_fsw_" + Date.now();
io.mkdir(dir);

// 1) the raw leaves: init -> dir -> drain packed records out of one Buf
const wfd = fsw.init();
if (!(wfd >= 0)) fail("fsw.init returned no fd");
fsw.dir(wfd, dir);
io.close(io.open(dir + "/hello.txt", "c"));

const buf = io.buf(4096);
const n = fsw.drain(wfd, buf);
if (n < 1) fail("fsw.drain reported no records");
const rows = fsw.records(buf);
eq(rows.length, n, "records parsed vs reported");
if (rows[0].name !== "") {                 // inotify: names are real
  if (!rows.some((r) => r.name === "hello.txt"))
    fail("created name absent: " + JSON.stringify(rows.map((r) => r.name)));
}
fsw.close(wfd);

// 2) an empty drain is 0 records, not a throw
const wfd2 = fsw.init();
fsw.dir(wfd2, dir);
eq(fsw.drain(wfd2, buf.reset()), 0, "idle drain");
fsw.close(wfd2);

// 3) the sugar: fsw.watch(dir, fn) over the pol loop
let got = null;
const w = fsw.watch(dir, (name, d) => {
  eq(d, dir, "handler dir");
  if (got === null) got = name;
  pol.stop();
});
io.close(io.open(dir + "/second.txt", "c"));
pol.after(3000, () => { pol.stop(); });    // safety net: never hang the suite
pol.run(pol.NEVER);
fsw.unwatch(w);
fsw.stop();                                // JAB-032: unwatch forgets one wd,
                                           // stop tears the shared wfd down
if (got === null) fail("fsw.watch handler never fired");
if (got !== "") eq(got, "second.txt", "watched name");
pol.init();                                // clear the sticky stop + table

// 4) JAB-032: TWO dirs on ONE wfd — the wd tells them apart.  This is the
// whole point of a watch descriptor: one watcher fd for a whole tree.
const dirA = dir + "/a", dirB = dir + "/b";
io.mkdir(dirA);
io.mkdir(dirB);
const wfd3 = fsw.init();
const wdA = fsw.dir(wfd3, dirA);
const wdB = fsw.dir(wfd3, dirB);
if (!(wdA > 0) || !(wdB > 0)) fail("fsw.dir returned no wd: " + wdA + "," + wdB);
if (wdA === wdB) fail("two dirs, one wd: " + wdA);
io.close(io.open(dirA + "/inA.txt", "c"));
io.close(io.open(dirB + "/inB.txt", "c"));
const n3 = fsw.drain(wfd3, buf.reset());
if (n3 < 2) fail("fsw.drain saw " + n3 + " records for two dirs");
const wds = {};
fsw.records(buf).forEach((r) => { wds[r.wd] = true; });
if (!wds[wdA]) fail("no record carried dirA's wd " + wdA);
if (!wds[wdB]) fail("no record carried dirB's wd " + wdB);
fsw.close(wfd3);

// 5) JAB-032: a queue overflow must be REPORTED (wd -1), never swallowed —
// a silent loss reads as "nothing changed" and poisons any cache.  One drain
// takes the whole queue, so the Buf is sized for max_queued_events records.
const dirC = dir + "/c";
io.mkdir(dirC);
const wfd4 = fsw.init();
fsw.dir(wfd4, dirC);
const QCAP = 16384;                        // fs.inotify.max_queued_events
for (let i = 0; i < QCAP; i++) io.close(io.open(dirC + "/f" + i, "c"));
const big = io.buf(1 << 21);
const n4 = fsw.drain(wfd4, big);
let over = false;
fsw.records(big).forEach((r) => { if (r.wd === fsw.OVERFLOW) over = true; });
if (!over) fail("" + QCAP + " creates drained " + n4 + " records, no overflow marker");
fsw.close(wfd4);

// 6) JAB-032: the sugar over ONE shared watcher — two dirs, two handlers,
// each event attributed to the dir it fell in.  This is the cache's path.
const dirD = dir + "/d", dirE = dir + "/e";
io.mkdir(dirD);
io.mkdir(dirE);
const hits = [];
const wD = fsw.watch(dirD, (name, d) => { hits.push([d, name]); pol.stop(); });
const wE = fsw.watch(dirE, (name, d) => { hits.push([d, name]); pol.stop(); });
if (wD === wE) fail("fsw.watch handed out one wd for two dirs: " + wD);
io.close(io.open(dirD + "/inD.txt", "c"));
pol.after(3000, () => { pol.stop(); });
pol.run(pol.NEVER);
if (!hits.length) fail("shared-watcher handler never fired");
eq(hits[0][0], dirD, "event attributed to the wrong dir");
if (hits[0][1] !== "") eq(hits[0][1], "inD.txt", "shared-watcher name");
fsw.unwatch(wD);
fsw.unwatch(wE);
fsw.stop();
pol.init();

io.rmdir(dir, true);
io.log("fsw.js OK");
