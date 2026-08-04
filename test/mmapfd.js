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

// ABC-023: a read-only map must keep NO fd — the JS idiom maps, reads and
// drops the view, and GC lags far behind a view that maps hundreds of files.
let sum = 0;
const want = 1999;                          // last value the ABA loop wrote
try {
    for (let i = 0; i < 2000; i++) {
        const m = abc.mmap("HASHu32", path, "r");
        sum += m[0] | 0;
    }
} catch (e) {
    fail("read-only map loop ran out of fds at " + fds() + ": " + e);
}
if (sum !== want * 2000) fail("read-only map read " + (sum / 2000));
const ro = fds();
if (ro > base + 4) fail("read-only fd leak: base=" + base + " after=" + ro);

// ABC-023: abc.close on a read-only map has no slot to find — must not throw.
for (let i = 0; i < 2000; i++) {
    const m = abc.mmap("HASHu32", path, "r");
    sum += m[0] | 0;
    abc.close(m);
}
const roclosed = fds();
if (roclosed > base + 4) fail("read-only close fd leak: base=" + base +
                              " after=" + roclosed);

// ABC-023: an EMPTY file has nothing to map (mmap of length 0 is EINVAL), so
// a read map of it is the empty record: a 0-length view, no throw, no fd.
const epath = "/tmp/jabc_mmapfd_empty.bin";
const efd = io.open(epath, "c");
io.resize(efd, 0);
io.close(efd);
for (let i = 0; i < 500; i++) {
    const d = io.mmap(epath, "r").data();
    if (!(d instanceof Uint8Array)) fail("empty map is not a Uint8Array");
    if (d.length !== 0) fail("empty map length " + d.length);
    if (d.slice().length !== 0) fail("empty map slice is not empty");
}
const roempty = fds();
if (roempty > base + 4) fail("empty map fd leak: base=" + base +
                             " after=" + roempty);

io.log("mmapfd.js OK (fds base=" + base + " after=" + after + " ro=" + ro +
       " roclosed=" + roclosed + " empty=" + roempty + " sum=" + sum + ")");
