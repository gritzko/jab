"use strict";
// io.chdir(path) (JS-108b): sets the process working directory over chdir(2);
// io.cwd() reflects it on success.  A missing path (ENOENT) or a non-directory
// (ENOTDIR) throws errno-mapped like the other io.* leaves and leaves the cwd
// unchanged.  Each assertion throws -> non-zero exit (CTest fails) — repro-first.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const start = io.cwd();
const dir = "/tmp/jabc_chdir_" + process.argv[0].length + "_" + Date.now();
io.mkdir(dir);

// chdir into the temp dir -> io.cwd() reflects the new dir
io.chdir(dir);
eq(io.cwd(), dir, "chdir sets cwd");

// a missing path throws (ENOENT) and leaves the cwd unchanged
{
  let threw = false;
  try { io.chdir(dir + "/nonexistent"); } catch (e) { threw = true; }
  if (!threw) fail("chdir(missing) did not throw");
  eq(io.cwd(), dir, "chdir(missing) left cwd unchanged");
}

// chdir onto a plain file throws (ENOTDIR), cwd unchanged
{
  const file = dir + "/file";
  { const fd = io.open(file, "c"); io.close(fd); }
  let threw = false;
  try { io.chdir(file); } catch (e) { threw = true; }
  if (!threw) fail("chdir(file) did not throw ENOTDIR");
  eq(io.cwd(), dir, "chdir(file) left cwd unchanged");
  io.unlink(file);
}

// chdir back to the starting dir restores it
io.chdir(start);
eq(io.cwd(), start, "chdir back to start");

// cleanup
io.rmdir(dir, true);

io.log("chdir.js OK");
