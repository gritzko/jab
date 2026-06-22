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

// cleanup
io.unlink(dangle);
io.unlink(link);
io.unlink(file);

io.log("stat.js OK");
