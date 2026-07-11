"use strict";
// JS-104: Socket.write FIFO growth must be amortized-linear — a synchronous
// write burst may trigger only O(log n) grows, not one grow per ~64K queued.
function fail(m) { throw "FAIL " + m; }

let grows = 0;
const srv = net.createServer(sock => {});          // peer never consumes fast
srv.listen(19310, "127.0.0.1", () => {
  const c = net.connect(19310, "127.0.0.1", () => {
    const wb = c._wb, orig = wb.grow.bind(wb);
    wb.grow = function (n) { grows++; return orig(n); };
    const chunk = new Uint8Array(64 << 10);
    const total = 16 << 20;                        // queue 16MB, no pol ticks
    for (let sent = 0; sent < total; sent += chunk.length) c.write(chunk);
    if (wb.size < total - chunk.length) fail("FIFO did not buffer: " + wb.size);
    io.log("grows=" + grows + " cap=" + wb.cap);
    if (grows > 24) fail("quadratic FIFO growth: " + grows + " grows for 16MB");
    c.destroy();
    srv.close();
    io.log("netgrow.js OK");
  });
});
