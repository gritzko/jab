"use strict";
// git.tree(bytes) pull cursor + git.parseCommit(bytes) eager object (JS-028).
// Pure marshalling over dog/git's GITu8sDrainTree / GITu8sParseCommit (no manual
// git framing in JS): the cursor + decode live in JS, the native side is a leaf.
// We hand-build a known tree blob (a regular file, a dir 40000, a gitlink 160000)
// and a known commit body, then assert the parsed entries/fields against vectors.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }
const dec = (u8) => utf8.Decode(u8);

// --- build a tree blob: (<octal-mode> <name>\0<20 raw sha bytes>)* -----------
// Three entries, git's canonical ordering (tree-sort: dir names sort with a '/'
// suffix, but we just lay them out; the parser is order-agnostic).  Distinct
// 20-byte shas so we can assert each.
function rawSha(seed) {                     // a deterministic 20-byte "sha"
  const b = new Uint8Array(20);
  for (let i = 0; i < 20; i++) b[i] = (seed + i * 7) & 0xff;
  return b;
}
function shaHex(seed) { return hex.encode(rawSha(seed)); }

function treeEntry(mode, name, shaSeed) {
  const head = utf8.Encode(mode + " " + name);
  const sha = rawSha(shaSeed);
  const e = new Uint8Array(head.length + 1 + 20);
  e.set(head, 0);
  e[head.length] = 0;                       // NUL terminator
  e.set(sha, head.length + 1);
  return e;
}
function concat(arrs) {
  let n = 0; for (const a of arrs) n += a.length;
  const out = new Uint8Array(n);
  let o = 0; for (const a of arrs) { out.set(a, o); o += a.length; }
  return out;
}

const entries = [
  { mode: "100644", name: "README.md",  seed: 0x11, modeNum: 0o100644 },
  { mode: "40000",  name: "src",        seed: 0x22, modeNum: 0o40000  },
  { mode: "160000", name: "vendor/lib", seed: 0x33, modeNum: 0o160000 },
  { mode: "120000", name: "link",       seed: 0x44, modeNum: 0o120000 },
];
const treeBytes = concat(entries.map((e) => treeEntry(e.mode, e.name, e.seed)));

// --- git.tree pull cursor ----------------------------------------------------
{
  const t = git.tree(treeBytes);
  let i = 0;
  while (t.next()) {
    const e = entries[i];
    eq(t.mode, e.modeNum, "entry " + i + " mode");
    eq(t.str, e.name, "entry " + i + " name (str)");
    eq(dec(t.name), e.name, "entry " + i + " name (zero-copy decode)");
    eq(t.sha, shaHex(e.seed), "entry " + i + " sha");
    // zero-copy: t.name must be a subarray of the source tree buffer
    if (!(t.name instanceof Uint8Array)) fail("entry " + i + " name not Uint8Array");
    if (t.name.buffer !== treeBytes.buffer) fail("entry " + i + " name not zero-copy");
    i++;
  }
  eq(i, entries.length, "full-walk entry count");
  // exhausted cursor stays false
  eq(t.next(), false, "next() false past end");

  // the 160000 gitlink mode is visible (submodule detection downstream)
  const t2 = git.tree(treeBytes);
  let sawGitlink = false;
  while (t2.next()) if (t2.mode === 0o160000) sawGitlink = true;
  if (!sawGitlink) fail("160000 gitlink mode not exposed");
}

// --- git.tree(bytes, cb) in-frame callback (io.readdir style) ----------------
{
  const seen = [];
  const r = git.tree(treeBytes, function (e) {
    seen.push({ mode: e.mode, name: e.str, sha: e.sha });
  });
  eq(r, undefined, "tree(cb) returns undefined");
  eq(seen.length, entries.length, "callback fired per entry");
  for (let i = 0; i < entries.length; i++) {
    eq(seen[i].mode, entries[i].modeNum, "cb entry " + i + " mode");
    eq(seen[i].name, entries[i].name, "cb entry " + i + " name");
    eq(seen[i].sha, shaHex(entries[i].seed), "cb entry " + i + " sha");
  }
}

// empty tree -> zero entries
{
  const t = git.tree(new Uint8Array(0));
  eq(t.next(), false, "empty tree next() false");
}

// --- git.parseCommit eager object --------------------------------------------
{
  const treeHex     = "1".repeat(40);
  const parent1Hex  = "a".repeat(40);
  const parent2Hex  = "b".repeat(40);
  const fosterHex   = "c".repeat(40);
  const author    = "Alice <alice@example.com> 1700000000 +0000";
  const committer = "Bob <bob@example.com> 1700000100 -0500";
  const subject   = "first line subject";
  const bodyText  = subject + "\n\nA longer paragraph.\n";
  const commitStr =
    "tree " + treeHex + "\n" +
    "parent " + parent1Hex + "\n" +
    "parent " + parent2Hex + "\n" +
    "author " + author + "\n" +
    "committer " + committer + "\n" +
    "foster " + fosterHex + "\n" +
    "\n" +
    bodyText;
  const commitBytes = utf8.Encode(commitStr);

  const c = git.parseCommit(commitBytes);
  eq(c.tree, treeHex, "commit tree");
  if (!Array.isArray(c.parents)) fail("parents not array");
  eq(c.parents.length, 2, "parents count");
  eq(c.parents[0], parent1Hex, "parent 0");
  eq(c.parents[1], parent2Hex, "parent 1");
  if (!Array.isArray(c.foster)) fail("foster not array");
  eq(c.foster.length, 1, "foster count");
  eq(c.foster[0], fosterHex, "foster 0");
  eq(c.author, author, "commit author");
  eq(c.committer, committer, "commit committer");
  eq(c.body, bodyText, "commit body");
}

// commit with no parents / no foster -> empty arrays, fields still present
{
  const treeHex = "2".repeat(40);
  const commitStr =
    "tree " + treeHex + "\n" +
    "author A <a@x> 1700000000 +0000\n" +
    "committer A <a@x> 1700000000 +0000\n" +
    "\n" +
    "root commit\n";
  const c = git.parseCommit(utf8.Encode(commitStr));
  eq(c.tree, treeHex, "root tree");
  eq(c.parents.length, 0, "root no parents");
  eq(c.foster.length, 0, "root no foster");
  eq(c.body, "root commit\n", "root body");
}
// JS-109: the empty-tree and default-body fallbacks (no tree header / no blank
// line) must yield "" — and, per the ticket, not leak the JSStringRef doing it.
{
  const commitStr =
    "author A <a@x> 1700000000 +0000\n" +
    "committer A <a@x> 1700000000 +0000\n";   // no tree, no blank line
  for (let i = 0; i < 1000; i++) {            // amplify for the leak checker
    const c = git.parseCommit(utf8.Encode(commitStr));
    if (i) continue;
    eq(c.tree, "", "no-tree commit -> empty tree");
    eq(c.body, "", "no-blank-line commit -> default empty body");
    eq(c.parents.length, 0, "no-tree commit parents");
  }
}
// JS-108: an embedded NUL in the commit body must survive into the JS string
// (length-explicit conversion; was: the JSC copy stopped at the NUL).
{
  const head = "tree " + "3".repeat(40) +
    "\nauthor A <a@x> 1 +0000\ncommitter A <a@x> 1 +0000\n\n";
  const hb = utf8.Encode(head);
  const body = new Uint8Array([98, 0, 99, 10]);          // "b\0c\n"
  const buf = new Uint8Array(hb.length + body.length);
  buf.set(hb, 0); buf.set(body, hb.length);
  const c = git.parseCommit(buf);
  eq(c.body.length, 4, "NUL body length");
  eq(c.body.charCodeAt(1), 0, "NUL survives in body");
}
io.log("git.js: OK\n");
