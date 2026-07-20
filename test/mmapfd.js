"use strict";
// ABC-020: abc.close must release the fd (munmap + close), not defer to GC.
// Loop mmap+close on ONE path; the open-fd count must stay flat.  Before the
// fix every close leaked an fd and this ran into "No file descriptors".
function fail(m) { throw "FAIL " + m; }

const path = "/tmp/jabc_mmapfd.bin";
// /proc/self/fd on Linux; macOS has no procfs, but /dev/fd lists the same
// per-process open fds there.
const fdDir = (() => {
    try { io.readdir("/proc/self/fd"); return "/proc/self/fd"; }
    catch (e) { return "/dev/fd"; }
})();
const fds = () => io.readdir(fdDir).length;

//  prime the backing file, then take a baseline fd count
abc.close(abc.mmap("HASHu32", path, "c", 256));
const base = fds();

for (let i = 0; i < 2000; i++) {
    const m = abc.mmap("HASHu32", path, "rw");
    abc.close(m);
}
const after = fds();
if (after > base + 4) fail("fd leak: base=" + base + " after=" + after);

// ABC-020: fd-slot ABA — a dead husk's GC finalizer must NOT release a LIVE
// remap of the same fd number + same mmap base; segfaulted pre-generation-fix.
for (let i = 0; i < 2000; i++) {
    (function () { abc.close(abc.mmap("HASHu32", path, "rw")); })();  // husk
    const b = abc.mmap("HASHu32", path, "rw");   // same fd, typically same base
    new ArrayBuffer(1 << 14); ("x" + i).repeat(50);                   // GC churn
    b[0] = i;
    if ((b[0] | 0) !== i) fail("stale read at i=" + i);
    abc.close(b);
}

io.log("mmapfd.js OK (fds base=" + base + " after=" + after + ")");
