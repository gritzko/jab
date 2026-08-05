"use strict";
// JAB-033: a mapping pins a VMA (65530 per process) and, for "rw", a booked fd
// — costs the byte-only collector cannot see, so jab prices them itself.
// Map one small file in a loop, dropping every handle: both counts must
// plateau.  Before the fix the VMA count grew one per map, forever.
function fail(m) { throw "FAIL " + m; }

const path = "/tmp/jabc_mmapvma.bin";
const MARK = 0x4a;                                   // 'J', the byte we check

//  /proc/self/maps IS the VMA count; macOS has no procfs, and no other way to
//  count mappings, so the VMA half only runs on Linux (the fd half runs both).
const scratch = new Uint8Array(8 << 20);
let procMaps = true;
try { io.close(io.open("/proc/self/maps", "r")); } catch (e) { procMaps = false; }
function vmas() {
    const fd = io.open("/proc/self/maps", "r");
    let t = 0, v = scratch, n;
    while (v.length && (n = io._read(fd, v)) > 0) { t += n; v = v.subarray(n); }
    io.close(fd);
    let c = 0;
    for (let i = 0; i < t; i++) if (scratch[i] === 10) c++;
    return c;
}

//  A SMALL file, like the ULOG that wedged the pager: 168 bytes of mapping is
//  a rounding error to a byte-counting collector, but still one whole VMA.
const seed = new Uint8Array(168);
seed[0] = MARK;
const sfd = io.open(path, "c");
io.writeAll(sfd, seed);
io.close(sfd);

//  the leak: N read-only maps of ONE small file, every handle dropped.  A read
//  map has no fd and no slot (ABC-023), so GC is its whole release path.
if (procMaps) {
    const N = 20000;
    const base = vmas();
    let peak = 0;
    for (let i = 1; i <= N; i++) {
        const m = io.mmap(path, "r");
        if ((m.bytes[0] | 0) !== MARK) fail("read map lost its bytes at " + i);
        if (i % 2000 === 0) { const v = vmas(); if (v > peak) peak = v; }
    }
    //  wide bar: the fix peaks ~4 K over the floor, the leak reached 17 K here
    //  and 62 K in the wild ([/todo/BRO/BRO-046]).
    if (peak > base + 8192)
        fail("read maps pile up: " + base + " -> " + peak + " VMAs over " + N);
}

//  an "rw" map also books an fd, and there are only 1024 of those — this loop
//  threw "No file descriptors" before the fix.
const fdDir = (() => {
    try { io.readdir("/proc/self/fd"); return "/proc/self/fd"; }
    catch (e) { return "/dev/fd"; }
})();
const fds = () => io.readdir(fdDir).length;
const f0 = fds();
let fpeak = 0;
for (let i = 1; i <= 3000; i++) {
    const m = io.mmap(path, "rw");
    m.bytes[0] = MARK;
    if (i % 250 === 0) { const f = fds(); if (f > fpeak) fpeak = f; }
}
if (fpeak > f0 + 512)
    fail("booked fds pile up: " + f0 + " -> " + fpeak + " open");

io.unlink(path);
io.log("mmapvma.js OK\n");
