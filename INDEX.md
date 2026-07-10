#   js — JABC (JavaScriptCore bindings) module index

JABC is a thin, anti-bloat JavaScriptCore binding over stock `libjavascriptcore`: it holds no memory and no long-lived JS refs (rule #4). Only buffers (JS-owned `Uint8Array`s in a `Buf` cursor) and fds (`number`s) cross the boundary. The native layer is leaf-only (syscalls, fills, mmap); all cursor/format logic lives in JS. `WITH_JS` is ON by default and the runtime binary `jab` (JAB-001: renamed from `jabc`, divorced from `be`) is built into `${DOG_BIN_DIR}`; `jab <name>` resolves a script via its own upward `jsrc/`-scan (scripts live in `jsrc/` dirs). See [README.md]/[API.md]; loops in [POL.md]/[NET.md].

###  JABC.hpp — binding macros + shared decls

Registration/error glue plus the one boundary touch shared by every leaf.

 -  `JABC_FN` — declare a native callback; `JABC_THROW`/`JABC_UNDEF` are its error/empty-return forms.
 -  `JABC_API_OBJECT`/`JABC_API_FN` — register a fresh global object / attach a native method to it.
 -  `JABCBytesOf` — a typed array's offset-adjusted backing range as a `u8s` (adds `byteOffset`, fails on a detached buffer).
 -  `JSOfCString` — a small JS string value from a C string (error text / keys).
 -  install hooks — `JABC*Install`/`Uninstall` entry points main calls per module.

###  utf8.cpp — UTF-8 text + the bytes helper

Validated, length-explicit conversion between JS strings and caller-owned byte buffers.

 -  `utf8.encodeInto(str, dst)` — encode into a caller-owned `Uint8Array`, return `n`; stops on a boundary (lone surrogates → U+FFFD).
 -  `utf8.Decode(u8)` — validate then build a JS string (explicit length, so embedded NULs survive).

###  io.cpp — fds, read/write, mmap, process leaves

Raw syscall leaves over abc `FILE*`; no `File` object and no custody table (the caller owns every fd).

 -  `io.open`/`close`/`sync`/`size`/`resize`/`lock`/`unlock`/`stat` — fd lifecycle (`"r"|"rw"|"c"`).
 -  `io.stat`/`lstat` — `{size,mode,kind,mtime,atime}` (`mtime`/`atime` ron60 BigInt); `lstat` no-follow (dangling link OK). `readlink`/`symlink`/`chmod` round out the FILE metadata leaves (JS-042).
 -  `io.setMtime(path, ron60BigInt)` — stamp a file's atime+mtime (`FILESetMtime`/`utimensat` NOFOLLOW: a symlink stamps the link); round-trips `lstat().mtime` exactly (JS-047).
 -  `io.readdir(path[, cbOrOpts])` — scan a dir (dirs trail `/`); polymorphic 2nd arg (cb / `{recursive,callback,hidden}`), cb in-frame.
 -  `io._read`/`io._write` — one `read`/`write` of a typed array's bytes, return `n` (0 = EOF); cursor advance is the `Buf`'s job.
 -  `io._mmap`/`io._ram`/`io._msync` — file or anon mmap → `Uint8Array` (munmap on GC); flush a mapped view's pages.
 -  `io.cwd`/`getenv`/`unlink`/`rename`/`mkdir`/`rmdir` — cwd / env var / remove / atomic rename / mkdir-with-parents / rmdir (`recursive`=rm -rf, over FILERmDir; lets checkout drop a dir on a type-change).
 -  `io.spawn`/`spawnFds`/`reap` — process leaves (JS-020): spawn → `{pid,stdin,stdout}`/pid, reap → `{code}`/`{signal}` (fds/pids are numbers).
 -  `io.log` — write strings / typed arrays to stderr.

###  buf.cpp — the `Buf` cursor class + constructors

An embedded JS bundle: a `Buf` wraps a `Uint8Array` plus its `PAST|DATA|IDLE` boundaries, the one cursor abstraction over all backings.

 -  `Buf` — `feed`/`feed1`/`feedStr`/`fed` (append), `take`/`skip` (consume), `data`/`idle`/`past` (views), `shed`/`pop`/`reset`/`shift`/`splice`/`grow`/`msync`.
 -  `io.buf`/`io.ram`/`io.mmap` — `Buf` constructors over heap / anon mmap / a file.
 -  `io.read`/`write`/`readAll`/`writeAll`/`readv`/`writev` — I/O dispatch over a `Buf` or a bare `Uint8Array`.

###  console.cpp — Node-style `console` (pure JS, JAB-002)

A `console` global layering over `utf8.Encode` + `io.writeAll` (holds no native code); log/info/debug → stdout (1), warn/error → stderr (2).

 -  `console.log`/`info`/`debug` — format the args and write a line to stdout (fd 1); `warn`/`error` write to stderr (fd 2).
 -  `format(args)` — multi-arg space-join + `%s %d %i %f %j %o %O %c %%` expansion (`%j` is JSON, `%o` is `inspect`); leftover args appended.
 -  `inspect(v)` — JSON-ish stringify: bareword keys, single-quoted strings, `[Circular]` on a cycle, `[Function: name]`; non-strings stringified.
 -  `console.trace`/`assert`/`dir` — `Trace:`-prefixed stderr line / `Assertion failed`-on-falsy stderr / `inspect` one value to stdout.

###  cont.cpp — container framework over abc logs

Per-(family,lane) JS prototypes bound once to pure-marshalling native leaves, plus the mmap constructors (`abc.ram`/`mmap`/`over`/`book`) and the `git` package; families are HEAP/HASH/HUNK/ULOG/WEAVE (PACK migrated to `git`).

 -  `abc.ram`/`mmap`/`over`/`book` — build a container over anon mmap / a file / an existing view / a sparse book file.
 -  `abc.merge`/`abc.intersect` — k-way merge / intersect of sorted inputs into an out region.
 -  `abc.index(lane, {dir,ext,mem})` — see `index.hpp`; the mmap LSM constructor.
 -  `git.pack.{ram,over,mmap,book}` — see `pack.hpp`; the offset-pure PACK container.
 -  `git.delta.apply(base, delta, out)` — see `pack.hpp`; reconstruct a delta target into an out `Buf`.
 -  `git.tree(bytes[, cb])` — pull cursor over a tree blob: `.next()` → `.mode`/`.name` (zero-copy)/`.str`/`.sha`; `cb` form runs in-frame.
 -  `git.parseCommit(bytes)` — eager parse → `{tree, parents[], foster[], author, committer, body}` (hex shas, decoded idents).
 -  `abc.weaveIdHash(hash, ord)` — WEAVE token identity hash, `hashlet(RAPHash(commit-id ++ ordinal))`.

###  index.hpp — INDEX binding: the mmap LSM (JS-022)

The 3 backing-agnostic native leaves an `abc.index` rides, plus the constructor (whose write path is pure JS); lanes `u64` and `wh128` (`kv64` deferred).

 -  `_findge_<lane>`/`_seekrange_<lane>`/`_compact_<lane>` — binary-search point, range/prefix drain, 1/8-ladder compaction leaf.
 -  `abc.index(...)` — a stack of oldest-first sorted runs (`.runs`) + a memtable, on-disk (`dir`) or in-memory (anon `io.ram`).
 -  `.put`/`.flush`/`.compact`/`.get` — write to memtable, merge into a fresh run, compact, point lookup.
 -  `.range`/`.prefix` — stream hits through an in-frame cb (no cross-run dedup; collapse is compaction's job).
 -  `.seek(needle)` — pure-JS pull cursor; `.next()` yields one merged entry (min-heap + newest-wins), exposing `.key`/`.val`/`.entry`.

###  pack.hpp — PACK + git object parsers (GIT-007/010, JS-028)

Pure marshalling over dog/git: the offset-addressed pack core, the delta op, the pack→index EMIT pair, and the git tree/commit parsers (all re-homed under the `git` package, see cont.cpp).

 -  `_feed`/`_resolve`/`_next`/`_delt_apply` — record feed, OFS base resolve, record end, delta apply (sha→offset index stays in JS).
 -  `PACK_PROTO` — header/feed/inflate/resolve/seek/next/finish + count/type/size/baseOffset/ref/offset on a `git.pack`.
 -  `_pack_scan`/`_pack_feed_emit` — one wh128 `(hashlet60|type → offset)` per object (`pack.scan`) or append (`pack.feed`) for an index lane.
 -  `_git_tree_next(bytes, off)` — drain ONE tree entry → `{mode,nameStart,nameEnd,sha,nextOff}`, backing `git.tree`.
 -  `_git_parse_commit(bytes)` — over `GITu8sDrainCommit`+`GITu8sCommitTree` → the `git.parseCommit` object.

###  ulog.hpp — ULOG binding over dog/ULOG (offset codec + booked log)

The stateless row codec over a JS `Uint8Array` (the `abc.ram("ULOG")` cursor family) PLUS the native file-backed log leaves (booked mmap + `wh128` sidecar index), whose (data, idx) pair is boxed on the heap and handed back as a Number handle (ULOG-002/001, approach-(a) prep).

 -  `_ulog_feed`/`_ulog_next`/`_ulog_now` — stateless append / row-end / fresh monotonic `ron60`, over a JS buffer at an offset.
 -  `_ulog_time`/`_ulog_verb`/`_ulog_uri` + `_ulog_seek{Verb,Time,URI}` — per-row fields and the six offset-pure forward/reverse seeks.
 -  `_ulog_open(path)`→handle / `_ulog_close(handle)` — `ULOGOpen`/`ULOGClose` a booked log; close trims to PAST+DATA (drops the zero pad) and frees the box.
 -  `_ulog_append(handle, ts, verb, uri)` — `ULOGAppendAt` (arg order = the on-disk row `<ts>\t<verb>\t<uri>`), ts-preserving + monotonic (stale ts → ULOGCLOCK throw); `_ulog_count`/`_ulog_rowUri`/`_ulog_rowTime` read back rows.

###  weave.hpp — WEAVE binding over dog/WEAVE

A WEAVE container is a u8 buffer holding ONE 'W' blob, parsed zero-copy per call; commit ids cross as 16-char hex hashlet strings (all u64↔hex in the leaf).

 -  `fold`/`merge` — `WEAVENext`/`WEAVEMerge`, rewrite the whole blob (fold a diff in / merge revs).
 -  `alive`/`produce`/`scope` — `WEAVEAlive`/`WEAVEProduce`/`WEAVEScope`: the live text, per-rev produce, scope bitmaps.
 -  `rewind`/`next` — the `WEAVEStep` token cursor.
 -  `emitDiff`/`emitFull` — append diff `'H'` records into a HUNK container; the C callback is the sink (no JS closure).
 -  `merged` — `WEAVEEmitMerged` renders an N-side merge into a `Buf`, framing divergent runs with the standard git-style conflict fences.

###  tok.cpp / tok.hpp — generic source tokenizer (JS-023)

The JS face of `dog/tok`'s `TOKLexer`, sharing the SAME core as `HUNK.dogenize` run straight into the caller's region (no duplicated parse logic).

 -  `tok._tok_parse_into(src, lang, outU8)` — the one leaf, lexes into `outU8` → count; guards (16 MiB, align, capacity) fire pre-write.
 -  `tok.parse(bytes, lang, out?)` — no `out` → a fresh trimmed `Uint32Array`; an `out` `Buf` → lex into `out.idle()`, advance `fed`, zero-copy.
 -  `TokStream` — pure-JS cursor decoding `tag`/`custom`/`side`/`end` by bit math; `text(src)` zero-copy, `str(src)` decodes (source pinned).

###  codec.cpp — byte transforms + `ron60` codec (hex / sha / ron / time)

Pure stateless transforms holding no refs, plus the `ron60`-as-timestamp codec (JS-021); `ron60` spans 2000-2099 only.

 -  `hex.encode`/`decode`/`encodeInto` — abc/HEX byte↔hex transforms.
 -  `sha1`/`sha256` — dog/git SHA1 + abc/SHA into the sha lane.
 -  `ron.encode`/`ron.decode` — `ron60`↔BigInt (RON base64).
 -  `ron.now()`/`ron.of(Date|ms)`/`ron.date(r)` — a `ron60` as a ULOG `ts`: now, from a Date/ms, the relative `be log` string (localtime).

###  zip.cpp — raw zlib (de)compression over dog/git/ZINF (JS-035)

Pure marshalling over `ZINFDeflate`/`ZINFInflate`; the missing primitive for a pure-JS git-wire client (loose objects, REF_DELTA bodies). JS owns the out buffer; the leaf sizes nothing.

 -  `zip._deflate`/`_inflate(src, out, outOff)` — RAW leaves: (de)compress into `out` at `outOff`, return bytes produced.
 -  `zip.deflate(bytes[, out])`/`inflate(bytes[, out])` — sugar: no `out` → fresh sized Uint8Array (inflate grows-retries); `out` Buf → IDLE+`fed`, return n.

###  pol.cpp — `pol` event loop over abc/POL

The `poll(2)` loop, rule #4 kept: C holds NO per-fd JS closures — the `fd→handler` table lives in an embedded JS bundle, C routes through two protected refs. Contract in [POL.md].

 -  `pol.watch(fd, mask, handler)`/`more`/`unwatch` — per-fd interest + handler (returns next mask, `0` drops fd); `pol.default` for the rest.
 -  `pol.every(ms, fn)`/`after(ms, fn)`/`untimer` — periodic / one-shot timer (`fn` returns next ms; `≥1h` removes).
 -  `pol.run(ns)`/`stop`/`sleep`/`any`/`now` — drive (`pol.NEVER`=forever), break, sleep, query; a handler throw unwinds `run()`.
 -  `pol.init(maxfd)` — (re)size the fd table + clear state (refused mid-loop). Consts: `pol.IN/OUT/ERR/HUP/PRI/NVAL`, `pol.SEC/MS/NEVER`.

###  net.cpp — net/dgram + Node timers over pol

A Node-style async API on top of `pol`: native socket leaves return bare fds, the EventEmitter, per-socket `Buf`s, and the timer wheel live in the embedded JS bundle. Contract in [NET.md].

 -  `net.createServer(onConn)`/`server.listen(port,host?,cb?)`/`server.close()` — TCP accept loop over `TCPListen`/`TCPAccept`.
 -  `net.connect(port,host?,cb?)` → Socket — `.on('data'|'end'|'drain'|'error'|'close')`, `.write`, `.end` (FIN), `.destroy`.
 -  `dgram.createSocket(type,onMsg?)`/`.bind`/`.send`/`.on('message',...)` — UDP over `UDPBind`+`recvfrom`/`sendto`.
 -  `setTimeout`/`setInterval`/`clearTimeout`/`clearInterval`/`setImmediate` — a JS min-heap over the single `pol` timer.

###  uri.cpp — abc/URI bindings + the `URI` class

Parse/compose/escape over abc/URI; a URI is small text, so components cross as decoded JS strings (not zero-copy views).

 -  `uri._parse`/`_make`/`_esc`/`_unesc` — leaves: parse into 8 components, compose from parts, percent-escape / unescape.
 -  `URI` class — `new URI(text)` (`.scheme`/`.host`/`.path`/...), `URI.make(...)`, `URI.escape`/`unescape`, `toString()`.

###  ansi.cpp — ANSI colour helper (pure JS)

SGR escape-code wrappers, no abc/ANSI link; gate on `io.isatty(fd)` to fall back to plain (theme-matched colours use the hunk renderer instead).

 -  `ansi.bold`/`dim`/`italic`/`under`/`rev` — style wrappers, each `s` → `ESC[n m … ESC[0m`.
 -  `ansi.black`/`red`/`green`/`yellow`/`blue`/`magenta`/`cyan`/`white`/`grey` — colour wrappers; `ansi.reset`, `ansi.sgr(n)`.

###  tty.cpp — terminal control leaf (JS-053)

Raw-mode + winsize over abc/ANSI's `ANSI*` POSIX wrappers (beside `ANSIBgColor`, sharing the raw-mode dance), for the `bin/bro.js` pager.  STATELESS (rule #4): JS owns the saved termios; C holds no per-fd state.  Pair with `io.open("/dev/tty","rw")` for keystrokes.

 -  `tty.raw(fd)` → `Uint8Array` — enter raw (BRO.c flags) and RETURN the saved termios; `tty.cook(fd, saved)` restores it.
 -  `tty.size(fd?)` → `{rows, cols}` — `TIOCGWINSZ` (default fd is stdout); the pager re-queries it per input tick (no SIGWINCH).
 -  `tty.openpty()` → `{master, slave}` / `tty.setSize(fd, rows, cols)` — pty + `TIOCSWINSZ` test support (no `/dev/tty` under ctest).

###  require.cpp — synchronous CommonJS `require()`

Built entirely on the existing bindings (`io.mmap` to read source, `utf8.Decode`, `io.stat` to probe) — no engine module loader, no promises.

 -  `require(spec)` — resolve → mmap → wrap in a `Function` → eval → cache; inserted BEFORE eval so cycles see partial exports.
 -  `require.resolve`/`require.cache` — explicit path (`/`,`./`,`../`) resolves `.js`/`/index.js`; a BAREWORD scans UP for `jsrc/` (try `<jsrc>/<name>`,`<name>.js`; ceiling `$HOME/jsrc` else `/jsrc`). By-abspath cache; each module's `require` is bound to its dir.
 -  `__main(spec)` — JAB-001 `jab <bareword>` entry: `resolveJsrc`, patch `process.argv[1]` to the abspath (the `here` idiom), then load it.

###  main.cpp — context, module install, script runner

`main()` maps `ABC_BASS`, builds the context, installs the modules, runs `--eval`/script, then drains the loop.  The binary is `jab` (renamed from `jabc`, JAB-001).

 -  module install order — utf8 → io → buf → console → cont → tok → uri → codec → zip → ansi → tty → pol → net → require.
 -  script entry — JAB-001: an EXPLICIT path (`/`,`./`,`../`) runs the file directly (global eval); a BARE name sets `__mainSpec` and runs `__main` (the require machine, upward `jsrc/`-scan).
 -  argv exposure — `JABCInstallArgv` sets the global `args` (tokens after the script) + Node-ish `process.argv` (`["jab", script, ...]`).
 -  build stamp — the same `process` carries read-only `version`/`build`/`build_date` from `dog/VERSN` (`JABCProcVersn`); "unknown" off a git checkout.
 -  run + drain — `JABCRun` propagates an uncaught exception to the exit code, then `pol.run(pol.NEVER)` drains the loop (Node-like).
 -  teardown order — release the context BEFORE `FILECloseAll` so GC deallocators (munmap) run while FILE is alive.

##  Tests

 -  `test/jabc_test.cpp` — C++ harness over utf8/Buf/io + the `ron` time codec (`jabc_test`, ctest `JABCtestCpp`).
 -  ctest targets — one `JABC*` per module (`codec`/`tok`/`index`/`pack`/`git`/`weave`/`pol`/`net`/... + family tests), each a `test/*.js` on `jab` (the ctest NAMEs + internal `JABC*` symbols keep their `JABC` prefix).
 -  `lsan.supp` — suppresses JSC-internal singleton leaks by library name.
