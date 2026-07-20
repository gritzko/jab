"use strict";
// FILE metadata bindings (JS-042): io.stat mtime/atime, io.lstat (no follow),
// io.readlink, io.symlink, io.chmod.  Each assertion throws on failure, which
// JABCRun turns into a non-zero exit (CTest fails) — repro-first.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const dir = "/tmp/jabc_stat_" + process.argv[0].length + "_" + Date.now();
io.mkdir(dir);
const file = dir + "/file";
const link = dir + "/link";       // -> file (valid)
const dangle = dir + "/dangle";   // -> nowhere (dangling)

// a regular file with some bytes
{
  const fd = io.open(file, "c");
  io._write(fd, new Uint8Array([1, 2, 3, 4, 5]));
  io.close(fd);
}

// io.stat: existing fields + new mtime/atime (BigInt ron60, positive)
{
  const st = io.stat(file);
  eq(st.size, 5, "stat size");
  eq(st.kind, "reg", "stat kind");
  if (typeof st.mtime !== "bigint") fail("stat.mtime not bigint: " + typeof st.mtime);
  if (typeof st.atime !== "bigint") fail("stat.atime not bigint: " + typeof st.atime);
  if (st.mtime <= 0n) fail("stat.mtime not positive: " + st.mtime);
  // ron60 spans 2000-2099 only; a present-day mtime decodes to a non-"?" date.
  if (ron.date(st.mtime).trim() === "?") fail("stat.mtime undecodable");
}

// io.symlink + io.readlink round-trip the target verbatim
{
  io.symlink(file, link);
  eq(io.readlink(link), file, "readlink round-trip");
}

// io.realpath resolves the link and any symlinked prefix (/tmp on macOS) to
// one canonical spelling; a missing path throws (ENOENT), errno-mapped.
{
  eq(io.realpath(link), io.realpath(file), "realpath resolves link");
  const rp = io.realpath(file);
  if (rp[0] !== "/") fail("realpath not absolute: " + rp);
  let threw = false;
  try { io.realpath(dir + "/nonexistent"); } catch (e) { threw = true; }
  if (!threw) fail("realpath(missing) did not throw");
}

// io.stat follows the link (kind reg, same size); io.lstat does NOT (kind lnk)
{
  const sf = io.stat(link);
  eq(sf.kind, "reg", "stat follows link -> reg");
  eq(sf.size, 5, "stat follows link -> size");
  const sl = io.lstat(link);
  eq(sl.kind, "lnk", "lstat does not follow -> lnk");
}

// lstat a DANGLING link must NOT throw (lstat does not follow); reports lnk
{
  io.symlink(dir + "/nonexistent-target", dangle);
  let threw = false, k = null;
  try { k = io.lstat(dangle).kind; } catch (e) { threw = true; }
  if (threw) fail("lstat(dangling) threw");
  eq(k, "lnk", "lstat(dangling) kind");
  // stat (follows) on a dangling link SHOULD throw (ENOENT through the link)
  let statThrew = false;
  try { io.stat(dangle); } catch (e) { statThrew = true; }
  if (!statThrew) fail("stat(dangling) did not throw");
}

// io.chmod: set 0755, then the owner-exec bit is visible via stat().mode
{
  io.chmod(file, 0o755);
  const m = io.stat(file).mode;
  if ((m & 0o111) === 0) fail("chmod 0755: exec bits unset, mode=" + m.toString(8));
  if ((m & 0o700) !== 0o700) fail("chmod 0755: owner rwx unset, mode=" + m.toString(8));
  io.chmod(file, 0o644);
  if ((io.stat(file).mode & 0o111) !== 0) fail("chmod 0644: exec bits still set");
}

// io.setMtime (JS-047): stamp a ron60, lstat round-trips it EXACTLY.
// ron60 is ms-resolution localtime; FILESetMtime -> utimensat NOFOLLOW, and
// FILELStat reads it back via the symmetric localtime split, so the BigInt
// must come back bit-identical.
{
  const r = ron.of(1700000000000); // 2023-11-14, inside ron60's 2000-2099 YY
  io.setMtime(file, r);
  eq(io.lstat(file).mtime, r, "setMtime round-trip via lstat");
  // a second distinct stamp overwrites the first (no accidental no-op)
  const r2 = ron.of(1600000000000); // 2020-09-13
  io.setMtime(file, r2);
  eq(io.lstat(file).mtime, r2, "setMtime second stamp");
}

// NOFOLLOW: stamping the symlink `link` (-> file, made above) stamps the LINK,
// not its target.  Stamp file to r1, then link to r2; lstat(link)===r2 while
// stat(file) stays r1.
{
  const r1 = ron.of(1700000000000); // already file's mtime from the block above
  const r2 = ron.of(1500000000000); // 2017-07-14
  io.setMtime(file, r1);
  io.setMtime(link, r2);                   // stamps the link itself, NOFOLLOW
  eq(io.lstat(link).mtime, r2, "setMtime stamps the link (NOFOLLOW)");
  eq(io.stat(file).mtime, r1, "setMtime(link) did not touch the target");
}

// a missing path throws (errno-mapped), like the other io.* leaves
{
  let threw = false;
  try { io.setMtime(dir + "/nonexistent", ron.now()); } catch (e) { threw = true; }
  if (!threw) fail("setMtime(missing) did not throw");
}

// io.rmdir (GET-039): plain rmdir drops an empty dir; ENOTEMPTY on a populated
// one; recursive:true is rm -rf.  A missing path throws like the other leaves.
{
  const sub = dir + "/sub";
  io.mkdir(sub);                                  // empty
  io.rmdir(sub);                                  // plain rmdir of an empty dir
  let gone = false;
  try { io.lstat(sub); } catch (e) { gone = true; }
  if (!gone) fail("rmdir(empty) did not remove the dir");

  io.mkdir(sub);                                  // repopulate, non-empty
  { const fd = io.open(sub + "/inner", "c"); io.close(fd); }
  let threw = false;
  try { io.rmdir(sub); } catch (e) { threw = true; }
  if (!threw) fail("rmdir(non-empty) did not throw");

  io.mkdir(sub + "/deep");                        // nested + a file → rm -rf
  { const fd = io.open(sub + "/deep/leaf", "c"); io.close(fd); }
  io.rmdir(sub, true);                            // recursive removes the subtree
  let goneR = false;
  try { io.lstat(sub); } catch (e) { goneR = true; }
  if (!goneR) fail("rmdir(recursive) did not remove the subtree");

  let missThrew = false;
  try { io.rmdir(dir + "/nonexistent"); } catch (e) { missThrew = true; }
  if (!missThrew) fail("rmdir(missing) did not throw");
}

// cleanup
io.unlink(dangle);
io.unlink(link);
io.unlink(file);
io.rmdir(dir, true);              // GET-039: drop the test dir (was leaked before)

io.log("stat.js OK");
