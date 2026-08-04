"use strict";
// io.readdir directives — the cb-form return vocabulary, and "skip" in
// particular: prune THIS dir's subtree and keep scanning.  Before "skip" the
// only descent control was "enough"/false, which aborts the WHOLE walk, so an
// ignore-aware walk had to enumerate every entry and filter afterwards.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }
function has(s, v, m) { if (!s.has(v)) fail(m + ": missing " + v); }
function hasnt(s, v, m) { if (s.has(v)) fail(m + ": unexpected " + v); }

// fixture:  a/1 a/x/2  b/3 b/x/4  c/5 c/x/6  z0 z1  .dot/9
const root = "/tmp/jabc_readdir_" + process.argv[0].length + "_" + Date.now();
io.mkdir(root);
for (const d of ["a", "b", "c", ".dot"]) io.mkdir(root + "/" + d);
for (const d of ["a", "b", "c"]) io.mkdir(root + "/" + d + "/x");
function touch(p) { io.close(io.open(p, "c")); }
touch(root + "/z0"); touch(root + "/z1"); touch(root + "/.dot/9");
touch(root + "/a/1"); touch(root + "/a/x/2");
touch(root + "/b/3"); touch(root + "/b/x/4");
touch(root + "/c/5"); touch(root + "/c/x/6");

function visit(ret, opts) {
  const seen = new Set();
  const o = { recursive: true, hidden: true };
  for (const k in (opts || {})) o[k] = opts[k];
  o.callback = function (nm) { seen.add(nm); return ret(nm); };
  io.readdir(root, o);
  return seen;
}

// --- baseline: the array form sees the whole tree ---------------------------
{
  const all = io.readdir(root, { recursive: true, hidden: true });
  eq(all.length, 16, "array form: whole subtree");
}

// --- "skip" on a DIR prunes its subtree and the scan CONTINUES --------------
{
  const seen = visit((nm) => (nm === "b/" ? "skip" : "more"));
  has(seen, "b/", "skip dir: the pruned dir itself was still delivered");
  hasnt(seen, "b/3", "skip dir: pruned subtree");
  hasnt(seen, "b/x/", "skip dir: pruned subtree");
  hasnt(seen, "b/x/4", "skip dir: pruned subtree");
  // the whole POINT: unlike false/"enough", the siblings still get scanned
  has(seen, "a/", "skip dir: sibling dir");
  has(seen, "a/x/2", "skip dir: sibling subtree");
  has(seen, "c/x/6", "skip dir: sibling subtree");
  has(seen, "z0", "skip dir: sibling file");
  eq(seen.size, 13, "skip dir: exactly the b/ subtree is gone");
}

// --- "skip" on a FILE is a plain continue (nothing to descend) --------------
{
  const seen = visit((nm) => (nm === "z0" ? "skip" : "more"));
  eq(seen.size, 16, "skip file: nothing else is lost");
}

// --- "skip" prunes under hidden:false too (the hidden filter still runs) ----
{
  const seen = visit((nm) => (nm === "b/" ? "skip" : "more"), { hidden: false });
  hasnt(seen, ".dot/", "hidden:false still filters dotdirs");
  hasnt(seen, "b/x/4", "skip dir: pruned under hidden:false");
  has(seen, "a/x/2", "skip dir: sibling survives under hidden:false");
}

// --- REGRESSION: false / "enough" still abort the WHOLE scan ----------------
{
  let n = 0;
  const seen = visit(function (nm) { n++; return nm === "b/" ? false : "more"; });
  has(seen, "b/", "false: the aborting entry was delivered");
  if (n >= 16) fail("false must abort the scan, saw " + n + " entries");
}
{
  let n = 0;
  visit(function () { n++; return n >= 4 ? "enough" : "more"; });
  eq(n, 4, "enough: stops at the 4th entry");
}

// --- REGRESSION: "more" / true / undefined all continue ---------------------
for (const [label, ret] of [["more", () => "more"], ["true", () => true],
                            ["undefined", () => undefined]]) {
  eq(visit(ret).size, 16, label + ": continues over the whole tree");
}

io.rmdir(root, true);             // cleanup
io.log("readdir: ok\n");
