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
if (got === null) fail("fsw.watch handler never fired");
if (got !== "") eq(got, "second.txt", "watched name");
pol.init();                                // clear the sticky stop + table

io.rmdir(dir, true);
io.log("fsw.js OK");
