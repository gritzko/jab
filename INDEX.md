#   js — JABC (JavaScriptCore bindings) module index

JABC is a thin, anti-bloat JavaScriptCore binding: stock `libjavascriptcore`, the binding holds no memory and no long-lived JS references. Two entities cross the boundary — buffers (JS-owned `Uint8Array`s wrapped in a `Buf` cursor) and file descriptors (plain `number`s). The native layer is leaf-only (syscalls + typed-array fills + mmap); the cursor logic lives in JS. Rationale in [README.md], the JS-facing surface and the ABC→JS mapping in [API.md].

##  Native modules

###  JABC.hpp — binding macros + shared decls

`JABC_FN` defines a native callback; `JABC_THROW`/`JABC_UNDEF` are the error/return forms; `JABC_API_OBJECT`/`JABC_API_FN` register a global object / its methods. `JABCBytesOf` is the one boundary touch — a typed array's offset-adjusted backing range as a `u8s` (adds `byteOffset`, NULL-checks a detached buffer).

###  utf8.cpp — text + the bytes helper

 -  `utf8.encodeInto(str, dst)` — encode UTF-8 into a caller-owned `Uint8Array`, return `n`; stops on a code-point boundary (no half multibyte), lone surrogates → U+FFFD.
 -  `utf8.Decode(u8)` — validate (abc `utf8sValid`/`utf8sDrain32`) then build a JS string via `JSStringCreateWithCharacters` (explicit length, embedded NULs survive).
 -  `JSOfCString` / `JABCBytesOf` — small JS string from a C string; typed-array range (defined here, shared).

###  io.cpp — fds + read/write + mmap (no `File` object, no custody table)

 -  `io.open`/`close`/`sync`/`size`/`resize`/`lock`/`unlock`/`stat` — fd lifecycle over abc `FILE*` (`"r"|"rw"|"c"`).
 -  `io.readdir` — over `FILEScanDir`/`FILEDeepScanDir`, root-relative with dirs marked by a trailing `/` (no `.`/`..`). POLYMORPHIC 2nd arg: absent / a function (sugar for `{callback}`) / an options object `{recursive, callback, hidden}`. No callback → `string[]` (one level, or the flat full subtree under `recursive:true` via native `FILEDeepScanDir`); a callback → `undefined`, with `cb(name)` per entry returning `"more"`/truthy/undefined (continue), `"enough"`/false (stop the whole scan), or `"recur"` (descend a nested scan — a no-op once `recursive:true` already descends). `hidden` (default false) skips dotfile basenames and prunes hidden dirs via `FILESKIP` (omit + don't descend); `hidden:true` includes them. cb runs in-frame, never stashed (rule #4), a throw aborts and propagates; a non-function/non-object 2nd arg throws. Throws on a missing/non-dir path.
 -  `io._read`/`io._write` — one `read`/`write` of a typed array's bytes, return `n` (0 = EOF); the cursor advance is the JS `Buf`'s job.
 -  `io._mmap` — `FILEMapRO/RW/Create` → `Uint8Array` no-copy, `munmap` on GC (`JABCMapFree`).
 -  `io._ram` — anonymous `MAP_NORESERVE` mmap → `Uint8Array`, `munmap` on GC (`JABCRamFree`).
 -  `io._msync` — flush a mapped typed array's pages (raw `msync`, no descriptor lookup).
 -  `io.cwd`/`io.getenv` — process cwd (`FILEGetCwd`) / env var (`FILEGetEnv`, `undefined` if unset); pure marshalling, no held ref.
 -  `io.unlink` — remove a name from the filesystem (`FILEUnLink`); pure marshalling.
 -  `io.spawn`/`spawnFds`/`reap` — process leaves (JS-020), pure marshalling over `FILESpawn`/`FILESpawnFds`/`FILEReap`. argv (a JS `string[]`) → a `u8css` over per-call STACK scratch (NUL-terminated bytes parked in PAST per element); `path` → `JABCPath` (NUL-termed `path8b`). `io.spawn(path,argv)`→`{pid,stdin,stdout}` (pipe fds; stderr INHERITED); `io.spawnFds(path,argv,inFd,outFd)`→`pid` (`-1`=inherit); `io.reap(pid)`→`{code}` on clean exit (any status) or `{signal}` on `FILESIGNAL` (exactly one key). fds + pid cross as numbers; caller owns + closes every fd; no held JS ref, no reap-on-GC (rule #4).
 -  `io.log` — write strings / typed arrays to stderr.

###  buf.cpp — the `Buf` cursor class + constructors (embedded JS)

The `Buf` class and the public API are a raw-string-literal JS bundle (no js2c/node). `Buf` wraps a `Uint8Array` + the `PAST|DATA|IDLE` boundaries: `feed`/`feed1`/`feedStr`/`fed` (append), `take`/`skip` (consume), `data`/`idle`/`past` (no-copy views), `shed`/`pop`/`reset`/`shift`/`splice`/`grow`/`msync`. Constructors `io.buf` (heap), `io.ram` (anon mmap), `io.mmap` (file); `io.read`/`write`/`readAll`/`writeAll`/`readv`/`writev` dispatch over `Buf` or a bare `Uint8Array`. `io.book` and native vectored I/O are not wired yet (see Outcome in API/tickets).

###  cont.cpp — container framework over abc logs

Per-(family,lane) JS prototypes bound once to native leaves, plus the mmap constructors (`abc.ram`/`mmap`/`over`/`book`). Families: HEAP/HASH lanes, HUNK, ULOG, PACK, WEAVE. Each native leaf is pure marshalling — resolve a typed array to a `u8s` and call one abc/dog function; no format logic in the binding.

 -  `pack.hpp` — PACK binding (GIT-007), pure marshalling over the dog/git pack core: `_feed`→`PACKu8sFeedObj` (raw|OFS_DELTA decided there), `_resolve`→`PACKResolveOfs`, `_next`→`PACKRecordEnd`; sha→offset index + base resolution stay in JS.
 -  `weave.hpp` — WEAVE binding over dog/WEAVE: a WEAVE container is a u8 buffer holding ONE 'W' blob, parsed zero-copy per call. `fold`→`WEAVENext`, `merge`→`WEAVEMerge` rewrite the whole blob; `alive`/`produce`→`WEAVEAlive`/`WEAVEProduce`, `scope`→`WEAVEScope`, the `rewind`/`next` cursor→`WEAVEStep`. `emitDiff`/`emitFull`→`WEAVEEmitDiff`/`WEAVEEmitFull` append diff `'H'` records (toks carry the per-token side) into a HUNK container read by the HUNK cursor — the C callback IS the sink (rule #4, no JS closure); `merged`→`WEAVEEmitMerged` renders an N-side merge into a Buf, framing conflicts `<<<< |||| >>>>`. Commit ids cross as 16-char hex hashlet strings (hi64 of the commit sha1); all u64↔hex lives in the leaf.

###  tok.cpp / tok.hpp — generic source tokenizer (JS-023)

The JS face of `dog/tok`'s `TOKLexer`. ONE native leaf `tok._tok_parse_into(srcBytes, lang, outU8) -> tokenCount` shares the SAME core as `HUNK.dogenize` — `HUNKu32bTokenize` (`dog/HUNK.c`) run STRAIGHT into the caller's region `outU8` (no parse logic duplicated, no scratch copy). `lang` is an extension (the core strips a leading dot); unknown/empty falls back to plain text (`TXTTLexer`). Guards, all BEFORE any write: source `> 0xFFFFFF` (16 MiB, the 24-bit offset cap) throws; `outU8` not 4-byte aligned throws (a `tok32` view must be aligned); `outU8` smaller than the `(srcn+1)` tok32 worst case throws (so a partial lex can't corrupt a reused buffer); empty → empty array. The JS `tok.parse(bytes, lang, out?)` dispatches: without `out` it allocates a fresh worst-case `(srcLen+1)*4`-byte scratch, lexes into it, and returns a fresh trimmed `Uint32Array` (unchanged behavior); with `out` (a `Buf`) it lexes into `out.idle()`, advances `out.fed(n*4)`, and returns a ZERO-COPY `Uint32Array` view over the bytes just written — so one `Buf` is reusable across parses (`reset()` between, the `delt.apply(…,out)` convention). The pure-JS `TokStream` cursor (embedded `R"JS"` bundle) decodes `tag`/`custom`/`side`/`end` by bit math mirroring `tok32Tag`/`Offset`/`Side`; `start` = the previous token's `end` (0 for i=0). It PINS the source (offsets are positions): `text(src)` is a zero-copy `subarray`, `str(src)` decodes via `utf8.Decode`.

###  codec.cpp — byte transforms + `ron60` codec (hex / sha / ron)

Pure stateless transforms, no held refs. `hex.encode/decode/encodeInto` (abc/HEX), `sha1`/`sha256` (dog/git SHA1 + abc/SHA) → the sha lane. `ron.encode/decode` cross `ron60`↔BigInt (RON base64). `ron` time codec (JS-021) interprets a `ron60` as a ULOG `ts`: native leaves `_now`→`RONNow`, `_ofMs(ms)`→`localtime`+`RONOfTime`, `_date(ron60)`→`RONToTime`+`mktime`+`DOGutf8sFeedDate`; the embedded `JABC_RON_JS` adds `ron.now()` (BigInt), `ron.of(Date|ms)` (Date→`getTime()`), `ron.date(r)`→relative string (`be log` "12:34"/"Tue05" form). All localtime-aligned like `RONNow`; `ron60` only spans 2000-2099 (`RONOfTime` throws out of range).

###  pol.cpp — `pol` event loop over abc/POL (one trampoline, JS owns the table)

The `poll(2)` loop binds `abc/POL` keeping JABC rule #4: C holds NO per-fd JS closures. The `fd→handler` table + wrappers live in an embedded JS bundle (like `Buf`); C holds only two protected router refs (`pol._fd`/`pol._timer`) and routes every ready fd / timer tick through them. `pol` carries readiness; handlers do their own `io.*` I/O. v1 = one timer (POL keys timers by C callback pointer). API + contract in [POL.md].

 -  `pol.watch(fd, mask, handler)` / `more` / `unwatch` — per-fd interest + handler; handler returns the next mask (`0` drops the fd). `pol.default` catches handler-less fds.
 -  `pol.every(ms, fn)` / `pol.after(ms, fn)` / `pol.untimer` — periodic / one-shot timer (`fn` returns next ms; `≥1h` removes).
 -  `pol.run(ns)` / `pol.stop` / `pol.sleep` / `pol.any` / `pol.now` — drive (`pol.NEVER`=forever), break, sleep, query; a handler throw unwinds out of `run()`.
 -  `pol.init(maxfd)` — (re)size the fd table + clear state; refused from inside a running loop. Consts: `pol.IN/OUT/ERR/HUP/PRI/NVAL`, `pol.SEC/MS/NEVER`.

###  net.cpp — net/dgram + Node timers over pol (sockets are fds)

Node-style async API on top of `pol`: native socket leaves return bare fds (EAGAIN → `-1`), and the EventEmitter, per-socket read/write `Buf`s, and the `setTimeout`/`setInterval` timer wheel all live in the embedded JS bundle. A socket is an fd registered with `pol.watch`; readiness drives the events; transfers are `recv`/`send`. Full surface + contract in [NET.md].

 -  `net.createServer(onConn)` / `server.listen(port, host?, cb?)` / `server.close()` — TCP accept loop over `TCPListen`/`TCPAccept`.
 -  `net.connect(port, host?, cb?)` → Socket; `.on('data'|'end'|'drain'|'error'|'close')`, `.write`, `.end` (FIN via `shutdown(SHUT_WR)`), `.destroy`.
 -  `dgram.createSocket(type, onMsg?)` / `.bind` / `.send(data, port, host)` / `.on('message',(msg,rinfo))` — UDP over `UDPBind`+`recvfrom`/`sendto`.
 -  `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`/`setImmediate` — a JS min-heap over the single `pol.timer`. Native leaves: `net._listen/_connect/_accept/_recv/_send/_shutwr/_close`, `dgram._bind/_recv/_send`.

###  main.cpp — context, module install, script runner

`main()` maps `ABC_BASS`, builds the context, installs utf8→io→buf→…→pol→net, exposes the script's argv tail as the globals `args` (tokens after the script path) + Node-ish `process.argv` (`["jabc", script, …tail]`, both via `JABCInstallArgv`, no held refs), runs `--eval`/script (propagating an uncaught exception to the exit code via `JABCRun`), then drains the event loop (`pol.run(pol.NEVER)`, Node-like) before releasing the context BEFORE `FILECloseAll` so GC deallocators run while the FILE subsystem is alive.

##  Tests

 -  `test/jabc_test.cpp` — table-driven C++ harness over utf8/Buf/io + the `ron` time codec (JS-021: `now`/`of`/`date`, links codec.cpp + dog); exits non-zero on any failed row. Build target `jabc_test`, ctest `JABCtestCpp`.
 -  ctest `JABCcodec` — `test/codec.js`: hex/sha vectors, `ron60` round-trips, and the JS-021 `ron.now`/`of`/`date` time codec against the real `jabc` binary.
 -  ctest `JABCtok` — `test/tok.js`: `tok.parse` of a `"js"` snippet (tag/start/end/text/str of known tokens, full-walk count), zero-copy `text()`, unknown/empty lang plain-text fallback, empty → empty, and a >16 MiB source throws. The `out` path: `tok.parse(src,lang,out)` matches the no-out result token-for-token, the returned view is zero-copy over `out` (cursor advanced `n*4`), one `out` Buf is reused across two parses (`reset()` between), and a too-small or unaligned `out` throws.
 -  ctest `JABCe2e` — the `jabc` binary runs an inline Buf+utf8 round-trip; a failed assertion throws → non-zero exit.
 -  ctest `JABCpol` — `test/pol.js`: periodic/one-shot timers, fd readiness (regular-file POLLIN, read-to-EOF drop), `pol.default`, handler-throw propagation, `pol.stop`.
 -  ctest `JABCnet` — `test/net.js`: TCP echo round-trip, a 200 KB multi-chunk transfer, UDP ping/pong, and setTimeout/clearTimeout/setInterval — all in the one implicit loop.
 -  ctest `JABCargv` — `test/run-015.sh`: `jabc script.js a b c` exposes `args` == `["a","b","c"]` and Node-shaped `process.argv`; `--eval` leaves `args` empty.
 -  ctest `JABCpack` — `test/pack.js`: offset-addressed pack write/seek/walk + the GIT-007 cross-impl vector (a JABC-written log resolves byte-identically through the dog/git multi-hop OFS_DELTA chase).
 -  ctest `JABCweave` — `test/weave.js`: from-blob round-trip (alive == blob), diff fold with scope-classified produce per rev, fork/merge of disjoint edits, the rewind/next token cursor, emitDiff/emitFull into a HUNK container, merged conflict/disjoint framing, and weaveIdHash — mirrors dog/test/WEAVE01.c + WEAVE02.c.
 -  `lsan.supp` — suppresses JSC-internal singleton leaks by library name.
