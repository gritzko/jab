#   JABC I/O API

Two entities cross the JS↔C boundary: **buffers** (bytes) and **file
descriptors** (fds). Buffers are JS-owned `Uint8Array`s wrapped in a `Buf`
cursor; fds are plain `number`s. The binding holds no memory and no JS
references beyond bootstrap — every persistent byte is owned by JS GC.

##  Principles

  - **Buffer = JS-owned bytes.** A `Buf` wraps a `Uint8Array` plus a cursor;
    the bytes are an engine `ArrayBuffer` (`io.buf`), an anonymous mapping
    (`io.ram`), or a file mapping (`io.mmap`/`io.book`). The binding allocates
    nothing that outlives a call and frees nothing by hand.
  - **fd = number.** No `File` wrapper object, no fd→object custody table. A
    fd is opened, passed to functions, and closed explicitly.
  - **The cursor lives in `Buf`, never in your code.** You call
    `feed`/`read`/`take`; the object moves its own boundaries. No offset
    arithmetic, mirroring how ABC keeps the cursor out of every C call site.
  - **Slicing is zero-copy.** `b.data()` / `b.idle()` return `Uint8Array`
    views over the same backing store. "Consume" is a view, never a C pointer.
  - **Errors throw.** A failed syscall maps `errno`→ABC `ok64`→a JS exception.
    Reads return the byte count; `0` means EOF.
  - **Copies are explicit.** Nothing crosses the boundary by value. The one
    place C memory reaches JS is a mapping (`io.mmap`/`io.ram`), handed over
    no-copy with a `munmap`/`free` deallocator the GC fires — because
    page-cache and anonymous maps can't be engine `ArrayBuffer`s.

##  The buffer model

A `Buf` is three regions over one fixed-capacity backing array, exactly ABC's
`u8b` (`abc/Bx.h`, `PAST | DATA | IDLE`):

```
  | PAST .... | DATA .... | IDLE .... |
  0          data        idle        cap
```

  - **PAST** `[0, data)` — bytes already consumed (`take`/`write`).
  - **DATA** `[data, idle)` — the live payload.
  - **IDLE** `[idle, cap)` — free space appends (`feed`/`read`) flow into.

`feed`/`read` grow DATA into IDLE; `take`/`write` shrink DATA from the PAST
side. So a `Buf` is a FIFO: fill DATA, drain DATA.

##  Construction

```js
let b = io.buf(65536);          // engine ArrayBuffer, JS-owned        (u8bAllocate)
let r = io.ram(1 << 30);        // anonymous mmap, lazy, munmap on GC  (u8bMap)
let m = io.mmap("idx", "r");    // file map RO; DATA = whole file      (FILEMapRO)
let w = io.mmap("db", "rw");    // file map RW; msync to persist       (FILEMapRW)
let c = io.mmap("new","c",4096);// create + map at size                (FILEMapCreate)
let k = io.book("log", 1<<30, 4096); // reserve 1GB VA, 4KB live, stable base (FILEBookCreate)

let v = Buf.over(u8);           // wrap existing Uint8Array as DATA (read source)
let s = Buf.into(u8);           // wrap as empty DATA / all IDLE   (write sink)
```

A file/`ram` `Buf` is the same cursor object as a heap one — only the backing
and whether it can grow differ.

##  `Buf` — feed (append into IDLE, DATA grows)

```js
b.feed(src);        // append a Uint8Array or another Buf's DATA      (u8bFeed)
b.feed1(0x0a);      // append one byte                                (u8bFeed1)
b.feedStr("foo");   // utf8-encode str straight into IDLE, advance    (encodeInto + u8bFed)
```

Each returns the byte count, or throws `NOROOM` if IDLE can't hold it (for a
growable backing, call `b.grow()` first — see below). You normally ignore the
return; the cursor advanced itself.

##  `Buf` — consume (drop from the DATA front, PAST grows)

```js
let head = b.take(n);  // view of the n consumed bytes, then advance past them (u8bUsed)
b.skip(n);             // advance past n bytes without returning a view        (u8bUsed)
```

`take` is the parser primitive: peek-and-advance in one call. The returned
view is valid until the next mutation.

##  `Buf` — views (zero-copy `Uint8Array` over the backing)

```js
b.data();   // [data, idle)  — the live payload (hand to io.write)
b.idle();   // [idle, cap)   — free space (a read target / encodeInto dst)
b.past();   // [0, data)     — already-consumed bytes
b.bytes;    // the whole backing Uint8Array
```

A view reflects the cursor at the instant you take it; after `feed`/`take`,
re-fetch. After a `grow()` that relocates the backing, old views are stale —
re-fetch.

##  `Buf` — sizes & edits

```js
b.size;     // DATA length (live bytes)        (u8bDataLen)
b.room;     // IDLE length (free bytes)        (u8bIdleLen)
b.cap;      // total capacity
b.empty;    // b.size === 0

b.shed(n);  // un-append: roll IDLE head back into DATA   (u8bShed)
b.pop();    // drop the last DATA byte                    (u8bPop)
b.reset();  // empty DATA and IDLE back to start          (u8bReset)
b.shift();  // compact: move DATA to the front, drop PAST (u8bShift)
b.splice(off, cut, paste); // in-place edit within DATA   (u8bSplice)
```

If you wrote into `b.idle()` yourself (e.g. via a foreign API), commit it with
`b.fed(n)` to advance the IDLE head by `n` (the manual `u8bFed`).

##  `Buf` — memory (growable / mapped backings only)

```js
b.grow(newCap);   // io.buf/ram: realloc-or-mremap (base may move, views detach)
                  // io.book:    extend within reserved VA (base STABLE, views survive)
b.msync();        // flush dirty pages of an RW mapping          (FILEMSync)
b.trim();         // truncate the file to DATA length            (FILETrimMap)
b.close();        // drop the mapping/alloc now (else GC does it)
```

`io.mmap(...,"r")` can't grow. `io.book` is the way to append to a file while
keeping every outstanding view valid.

##  fds

```js
let fd = io.open("data", "rw");   // "r" | "rw" | "c" (create)     (FILEOpen/Create)
io.close(fd);                                                       // (FILEClose)
io.sync(fd);                      // fsync                          (FILESync)
io.size(fd);                      // → number                      (FILESize)
io.resize(fd, n);                                                   // (FILEResize)
io.lock(fd, true); io.unlock(fd); // flock LOCK_EX / LOCK_UN        (FILELock)
let st = io.stat("data");         // {size,mode,kind,mtime,atime}  (FILEStat)
let ls = io.lstat("link");        // same shape, no symlink follow (FILELStat)
io.symlink("data", "link");       // create a symlink → target     (FILESymLink)
let tg = io.readlink("link");     // → target string               (FILEReadLink)
io.chmod("data", 0o755);          // set POSIX perm bits           (FILEChmod)
io.setMtime("data", st.mtime);    // stamp atime+mtime (ron60 BigInt, NOFOLLOW) (FILESetMtime)
let ns = io.readdir("dir");       // → string[], dirs marked "x/"  (FILEScanDir)
io.readdir("dir", n => "more");   // cb scan: "more"/"enough"/"recur" directive
let all = io.readdir("dir", {recursive:true});  // → flat subtree   (FILEDeepScanDir)
let h = io.readdir("dir", {hidden:true});       // → incl. dotfiles ('.x')
io.readdir("dir", {recursive:true, callback:n=>{}}); // cb across the subtree
io.unlink("tmp");                 // remove a name                  (FILEUnLink)
io.rename("a.tmp", "a");          // atomic rename within a FS       (FILERename)
io.mkdir("a/b/c");                // create dir + parents (idempotent)(FILEMakeDirP)
io.rmdir("d", true);              // remove a dir (recursive=rm -rf)  (FILERmDir)
let dir = io.cwd();               // process working directory      (FILEGetCwd)
let h = io.getenv("HOME");        // env var, undefined if unset    (FILEGetEnv)
```

`io.readdir` names are root-relative; directories carry a trailing `/`
(`"alpha"`, `"sub/"`, recursively `"sub/child"`), so files vs dirs are
distinguishable. The 2nd arg is **polymorphic**: absent, a function (sugar for
`{callback:fn}`), or an options object `{recursive, callback, hidden}` (any
subset). With no callback it returns the `string[]` — one level, or the flat
full subtree under `recursive:true` (`FILEDeepScanDir`). With a callback it
returns `undefined` and runs `cb(name)` per entry inside the scan (never
stashed); the return directs traversal — `"more"`/truthy/`undefined` continue,
`"enough"`/`false` stop the whole scan, `"recur"` descends into the entry (a
dir) before the next sibling (a no-op once `recursive:true` already descends).
`hidden` (default `false`) skips dotfile basenames and does not descend hidden
dirs; `hidden:true` includes them. A `cb` throw aborts and propagates; a
2nd arg that is neither a function nor an object throws.

`io.stat`/`io.lstat` return `{size, mode, kind, mtime, atime}`: `size` bytes
and `mode` (POSIX rwx bits, `0700`/`0070`/`0007`, e.g. `st.mode & 0o111` for any
exec bit) are numbers, `kind` is `"reg"`/`"dir"`/`"lnk"`/`"other"`, and
`mtime`/`atime` are **BigInt** `ron60` timestamps — the same encoding
`ron.encode`/`ron.date`/the dateCol consume (`ron.date(st.mtime)` formats one).
`io.stat` follows symlinks (a link to a regular file is `"reg"`); `io.lstat`
does not (`"lnk"`), and stats a **dangling** link fine (no throw) since it
inspects the link itself, never its target. `io.readlink(path)` returns a link's
target string; `io.symlink(target, linkpath)` creates one (target stored
verbatim, may be relative/dangling; throws if linkpath exists); `io.chmod(path,
mode)` sets the permission bits (octal int, e.g. `0o755`). `io.setMtime(path,
ron60)` stamps a file's atime **and** mtime to a `ron60` BigInt instant (the
inverse of the `stat` read, so `io.lstat(path).mtime` round-trips it exactly);
it is `AT_SYMLINK_NOFOLLOW`, so stamping a symlink stamps the link, not its
target — the primitive the JS write verbs use to leave a freshly-staged file
looking clean to the next `be`.

##  feed/drain over fds

A `Buf` argument advances its own cursor; a bare `Uint8Array` does not (you
slice it). All return the byte count; reads return `0` at EOF.

```js
// write — drain DATA to the fd
io.write(fd, b);        // one write; consumes written bytes from DATA front (FILEFeed→u8bUsed)
io.writeAll(fd, b);     // loop until DATA is empty                          (FILEFeedAll)
io.writev(fd, [a, b]);  // scatter (writev)                                  (FILEFeedv)

// read — fill IDLE, DATA grows
io.read(fd, b);         // one read into IDLE; advances idle head            (FILEDrain→u8bFed)
io.readAll(fd, b);      // read until IDLE full or EOF                       (FILEdrainall)
io.readv(fd, [a, b]);   // gather (readv)                                    (FILEdrainv)
```

##  processes (spawn / reap)

Run a child process. The three native leaves are pure marshalling over
`FILESpawn`/`FILESpawnFds`/`FILEReap`; `argv` is a JS `string[]` (`argv[0]` is
the program name the child sees), `path` is resolved by `execvp` (an absolute
path is used as-is; a bare name triggers a `PATH` lookup). No shell. fds and
the pid cross as plain `number`s — you own and close every fd, and you reap the
pid yourself (the binding never reaps on GC).

```js
let {pid, stdin, stdout} = io.spawn("/bin/cat", ["cat"]);   // pipe fds   (FILESpawn)
//   stdin  = a WRITE fd to the child's stdin
//   stdout = a READ  fd from the child's stdout
//   stderr is INHERITED from the parent (goes to the parent's stderr)

let pid = io.spawnFds("/bin/cat", ["cat"], inFd, outFd);    // caller fds  (FILESpawnFds)
//   inFd / outFd become the child's stdin / stdout; `-1` inherits.
//   The child dups them; the caller still owns + closes them.

let r = io.reap(pid);   // wait for the child                              (FILEReap)
//   clean exit (any status)  → { code }    (e.g. {code:0}, {code:3})
//   killed by a signal       → { signal }  (e.g. {signal:9})
//   EXACTLY ONE key is set.  Any other failure throws.
```

**Pipe deadlock caveat.** A `spawn` stdout pipe holds ~64 KB. If the child
writes more than that before you drain `stdout`, it blocks — and if you reap
before draining, both sides hang. So drain the pipe to EOF *before* reaping:

```js
// Capture stdout into a growable Buf (single pipe → no deadlock).
let p = io.spawn("/usr/bin/git", ["git", "log", "--oneline"]);
io.close(p.stdin);                       // child reads no input
let b = io.ram(1 << 20);
while (io.read(p.stdout, b) > 0) {}       // drain to EOF (grows as needed)
io.close(p.stdout);
let {code} = io.reap(p.pid);              // b.data() = stdout
```

For a large output, sink stdout to a file and `mmap` it back zero-copy instead
(no pipe, no drain loop); `unlink` after the map and the inode auto-cleans on GC:

```js
let fd = io.open("/tmp/out", "c");
let pid = io.spawnFds("/usr/bin/git", ["git", "log"], -1, fd);  // stdout → file
io.close(fd); let {code} = io.reap(pid);
let out = io.mmap("/tmp/out", "r"); io.unlink("/tmp/out");      // zero-copy Buf
```

##  tty — terminal control (raw mode / winsize)

For an interactive pager (`bin/bro.js`). Over abc/ANSI's `ANSI*` POSIX
wrappers (next to `ANSIBgColor`, which shares the raw-mode dance). STATELESS
like every leaf: `tty.raw` RETURNS the saved termios as a
`Uint8Array` that JS holds; `tty.cook` takes those bytes back to restore. C
keeps no per-fd state, so YOU own the saved-state buffer and the
restore-on-exit safety (a `try/finally` around the loop). Keystrokes come from
`io.open("/dev/tty", "rw")` so input still works when stdin is a data pipe.

```js
let fd = io.open("/dev/tty", "rw");          // O_RDWR|O_NOCTTY under the hood
let saved = tty.raw(fd);                      // enter raw, → savedTermios (Uint8Array)
try {
  let {rows, cols} = tty.size(fd);            // ioctl TIOCGWINSZ; re-query per tick
  // ... interactive loop: io.read(fd, buf), paint, re-query size on change ...
} finally {
  tty.cook(fd, saved);                        // ALWAYS restore — a crash else wedges the shell
  io.close(fd);
}
```

`tty.raw` clears `ECHO|ICANON|ISIG|IEXTEN`, `IXON|ICRNL|BRKINT|INPCK|ISTRIP`,
`OPOST` and sets `VMIN=0 VTIME=1` (the bro/BRO.c set). `tty.size(fd?)` defaults
to stdout. `tty.openpty()` → `{master, slave}` and `tty.setSize(fd, rows, cols)`
are pty test support (no controlling tty exists under ctest).

##  text

```js
utf8.encodeInto(str, dst);   // encode into a Uint8Array you own, → n   (primary, no alloc)
utf8.Encode(str);            // → fresh Uint8Array (engine-owned)       (sugar over BASS scratch)
utf8.Decode(u8);             // validate UTF-8, → JS string; throws on malformed
```

`Decode` rejects overlong forms, lone surrogates, truncated multibyte and
> U+10FFFF (via ABC `utf8sValid`/`utf8sDrain32`); it is not JSC's lenient
`JSStringCreateWithUTF8CString`.

##  console — Node-style logging (JAB-002)

A pure-JS `console` over `utf8.Encode` + `io.writeAll`; no native code. `log`,
`info`, `debug` go to **stdout** (fd 1); `warn`, `error`, `trace`, `assert` to
**stderr** (fd 2). Each call formats its args, then writes one line + `"\n"`.

```js
console.log("hi", 1, [2, 3]);        // "hi 1 [ 2, 3 ]"   (multi-arg space-join)
console.log("%s=%d", "n", 5);        // "n=5"             (printf specifiers)
console.error("oops: %o", {e: 1});   // -> stderr         "oops: { e: 1 }"
console.warn("x"); console.info("y"); console.debug("z");
console.assert(cond, "msg");         // falsy -> "Assertion failed: msg" (stderr)
console.trace("here");               // "Trace: here"     (stderr)
console.dir({a: 1});                 // inspect one value (stdout)
```

Specifiers consume positional args left-to-right: `%s` string, `%d`/`%i`
integer (truncated), `%f` float, `%j` JSON (`[Circular]` on a cycle), `%o`/`%O`
`inspect`, `%c` CSS (consumed, no output), `%%` a literal `%`. Args left over
after the specifiers are space-appended; a specifier with no arg stays literal.
Objects render JSON-ish (bareword keys, single-quoted strings); a cycle is
`[Circular]`, a function `[Function: name]`.

##  ron — `ron60` time codec

```js
ron.encode(bigint);   // ron60 → RON base64 string   ron.decode(str) → BigInt
ron.now();            // current ron60 (BigInt), localtime-aligned ms (RONNow)
ron.of(Date|ms);      // a JS Date or ms-epoch int → ron60 (BigInt)
ron.date(ron60);      // → relative-date string: "12:34" / "Tue05" / "01Jan25"
```

`ron60` is the ULOG `ts` encoding; it crosses as a `BigInt`. `now`/`of` align
to the wall clock like `RONNow` (localtime), and only span **2000-2099** —
`ron.of` of an out-of-range instant throws. `ron.date` renders the same
relative form `be log`/`be status` use (`DOGutf8sFeedDate`), centre-padded to
7 columns; `ron.date(0n)` is the `"?"` placeholder.

##  zip — raw zlib (deflate / inflate)

```js
zip.deflate(bytes[, out]);  // → fresh Uint8Array, or write into an out Buf → n
zip.inflate(bytes[, out]);  // → fresh Uint8Array, or write into an out Buf → n
```

Pure marshalling over `dog/git/ZINF` (the same zlib the pack reader uses), the
standalone-stream primitive a pure-JS git-wire client needs (loose objects,
REF_DELTA bodies). With no `out` you get a freshly-sized `Uint8Array`; `inflate`
grows-and-retries when the result is bigger than its first guess. With an `out`
`Buf` the bytes land in IDLE, `fed` advances, and the byte count is returned
(zero-copy). JS owns the out buffer — the leaf sizes nothing. Bad zlib input
throws.

##  abc.index — mmap LSM index (point / range / prefix / seek)

A `abc.index(lane, {dir, ext, mem})` is a stack of immutable sorted runs
(`.runs`, oldest-first) plus a memtable. The write path is pure JS over the
existing leaves (sort + `abc.merge`/`book`/`close` + `io.rename`); the read path
and `compact` ride 3 native leaves (`<lane>sFindGE`, `HIT<lane>SeekRange`,
`HIT<lane>Compact`), all backing-agnostic (typed-array views).

```js
let ix = abc.index("u64", {dir:"idx", ext:".u64", mem:4096});  // or "wh128"
ix.put(v);                 // u64: one BigInt;  wh128: ix.put(key, val)
ix.feed(view);             // bulk put of a contiguous entries view (JS-106)
ix.flush();                // memtable.sort()→merge into a fresh run
ix.compact();              // 1/8 size-tiered ladder over .runs (full-elem dedup)
ix.get(needle);            // POINT: u64 scalar / wh128 KEY → value | undefined
ix.range(lo, hi, v => …);  // [lo,hi): u64 cb(v); wh128 keys, cb([key,val])
ix.prefix(p, lowBits, cb); // == range [p, p + 2^lowBits)  (spot/keeper block)
let c = ix.seek(needle);   // PULL cursor: u64 seek(v); wh128 seek(key[, val])
while (c.next()) use(c.key, c.val, c.entry);   // ascending from needle; stop anytime
```

**Two run backings, chosen by `opts.dir`:**
- **on-disk** (`dir` PRESENT) — runs are `abc.book` files in `<dir>`; `flush`
  books + msync-trims + `io.rename` + re-opens RO; `compact` lands the merged run
  on disk; re-opening an existing dir loads its run files. Persistent.
- **in-memory** (`dir` ABSENT) — runs live in anonymous `io.ram` mappings: NO
  files, no rename/re-open, GC munmaps them. Starts empty each open, gone at exit.
  `ix.onDisk` is `false`; `ix.dir` is `undefined`.

`.range`/`.prefix` STREAM hits through an in-frame callback (rule #4) — the cb
return is a stop signal (`false`/`"enough"` stop, truthy/`undefined` go on),
exactly the `io.readdir(path, cb)` contract — via the native `_seekrange_`
fast-drain (so it does NOT dedup across runs; collapse is a compaction concern).

`.seek(needle)` is a **PURE-JS PULL cursor** (rule #4 — ALL state in JS, no held
native cursor; only the per-source `_findge_` binary search crosses). It seeks
every source (each run + a snapshot of the current memtable) to the first entry
`>= needle`, then `.next()` advances ONE merged entry at a time in ascending
order (binary min-heap over the heads + **full-element newest-wins dedup**, the
same collapse as compaction/`.range`), exposing `.key`/`.val`/`.entry` (u64:
all three are the scalar; wh128: the key, the val, and the `[key,val]` pair).
`.next()` returns `false` past the last entry. The caller pulls as many as it
wants and stops anytime. The memtable is snapshotted on `seek` (its live puts are
visible exactly as to `get`/`range`; later puts are not reflected — no fork).

Lanes: **u64** (spot trigram, scalar) and **wh128** (keeper puppy registry,
`(key,val)`, point on KEY). u64 needles/`lo`/`hi` are BigInt; wh128 keys cross as
BigInt. `kv64` is deferred (its `Z` is key-only ⇒ compaction dedup is not
full-element). **v1 deletes**: a newer run shadows an older one AT QUERY time
(`get` scans runs newest-first; no tombstone drop). Compaction collapses
identical rows only, so a key with two different vals across runs keeps both
(keyed compaction is deferred) — keep one stable val per key. On open each run
is alignment-checked (`byteLength % (w·BPE) === 0`).

##  git — pack-log package (`git.pack` / `git.delta`)

The git/pack-log JS surface (JS-024). `git.pack` builds an OFFSET-ADDRESSED git
pack log (a `u8` buffer: 12-byte header + `[obj-hdr][zlib]` records); `git.delta`
is the delta op. Pure marshalling over the dog/git pack core — the native leaves
(`PACKu8sFeedObj`/`PACKResolveOfs`/…, `DELTApply`) are UNCHANGED, just re-homed
here (was `abc.over("PACK")` / `delt.apply`, now removed — hard cutover).

```js
let p = git.pack.ram(1 << 16);   // anon mmap; .over(ta) / .mmap(path,mode,slots) / .book(path,slots)
p.header();                      // write the 12-byte header (count filled by finish)
let a = p.feed("blob", v1);      // raw object -> byte offset a
let b = p.feed("blob", v2, a);   // base @a -> the writer picks raw|OFS_DELTA
p.finish();                      // backfill the object count

p.seek(a);                       // address by OFFSET only (a sha throws)
p.type; p.size; p.count;         // "blob" / size / object count
p.baseOffset; p.ref;             // ofs-delta base offset / ref-delta sha (else undefined)
p.inflate(out);   p.resolve(out); // one record's zlib bytes / the full delta-chase, into a Buf
p.rewind(); while (p.next()) use(p.offset, p.type);  // sequential walk

git.delta.apply(base, delta, out);  // reconstruct a delta target into a Buf, returns n
```

`git.pack.{ram,over,mmap,book}` mirror the abc-container constructors verbatim
(`ram(slots)` anon mmap, `over(ta)` wrap, `mmap(path,mode,slots)` file,
`book(path,slots)` sparse output); `abc.close` msyncs + unpins a booked pack
(trim to the write head, no lane scaling). A PACK never takes a sha — sha→offset
indexing is the index layer's job (`abc.index`). The GIT-007 cross-impl vector
holds: a JABC-written log resolves byte-identically through the dog/git multi-hop
OFS_DELTA chase (keeper + binding share one pack format).

###  git object parsers (`git.tree` / `git.parseCommit`)

The git tree + commit parsers (JS-028), pure marshalling over `dog/git`'s
`GITu8sDrainTree` / `GITu8sDrainCommit` / `GITu8sCommitTree` — NO manual git
framing in JS. `git.tree` is a PULL cursor (state in JS, rule #4); the native
leaf `abc._git_tree_next(bytes, off)` drains exactly one entry per call.

```js
let t = git.tree(treeBytes);             // tree blob: (<mode> <name>\0<20B sha>)*
while (t.next()) {                       // advance; false at end
  t.mode;   // octal number incl. 0o160000 gitlink / 0o40000 dir / 0o100644 file
  t.name;   // zero-copy Uint8Array subarray of the entry name (over treeBytes)
  t.str;    // utf8.Decode(t.name)
  t.sha;    // 40-hex string
}
git.tree(treeBytes, (e) => use(e.mode, e.str, e.sha));  // in-frame cb (readdir style)

let c = git.parseCommit(commitBytes);    // eager (commit objects are small)
c.tree;       // "<40hex>"          (via GITu8sCommitTree)
c.parents;    // ["<40hex>", …]     (every `parent` header, in order)
c.foster;     // ["<40hex>", …]     (beagle `foster` headers)
c.author;     // "Name <email> ts tz"   c.committer;   // same shape
c.body;       // message body string (text after the blank line)
```

`git.tree(bytes)` returns a `GitTree` cursor; `.next()` advances and exposes
`.mode`/`.name`/`.str`/`.sha`, returning `false` past the last entry (and on an
empty tree). `.name` is a zero-copy `subarray` of the source bytes (so the cursor
PINS `bytes`). `git.tree(bytes, cb)` drives the cursor in-frame and calls `cb(t)`
per entry (the cursor itself is the argument), returning `undefined`. The
`0o160000` mode is surfaced so submodule gitlinks are detectable downstream.
`git.parseCommit` walks the header block once (`GITu8sDrainCommit`) collecting
parents/foster and the author/committer ident lines, reads the tree sha via
`GITu8sCommitTree`, and returns the message body — all hex shas as lowercase
40-char strings.

##  tok — generic source tokenizer

```js
let t32 = tok.parse(srcBytes, "js");   // Uint8Array + ext -> Uint32Array of tok32
let t  = tok.parse(srcBytes, "js", out); // into a Buf: zero-copy view, reuse out
let s = new TokStream(t32, srcBytes);  // cursor; pins srcBytes (offsets = positions)
s.tag; s.start; s.end; s.side; s.custom; // current token (tag is a 1-char string)
s.text(src);   // zero-copy Uint8Array subarray over src  (defaults to the pinned src)
s.str(src);    // utf8.Decode of text()
s.next();      // advance; false past the last token       s.seek(i); s.length;
```

`tok.parse` drives the same `dog/tok` lexer as `HUNK.dogenize` (no parse logic
in JS), returning the bare `tok32` array instead of a hunk. `lang` is an
extension (a leading dot is fine); unknown/empty falls back to plain text. A
source `> 0xFFFFFF` bytes (16 MiB, the 24-bit end-offset cap) throws; empty →
empty array. Without `out`, a fresh (4-aligned) `Uint32Array` is returned. With
`out` (a `Buf`), the packed `tok32` is written into `out`'s IDLE, `out.fed(n*4)`
advances its cursor, and a **zero-copy** `Uint32Array` view over those bytes is
returned — so one `Buf` can be reused across many parses (`reset()` between),
the `git.delta.apply(…,out)` convention. The reused `Buf`'s IDLE head must be
4-aligned (a fresh/reset `Buf` is) and large enough for `(srcLen+1)` tok32, else
`tok.parse` throws. `TokStream` decodes by bit math — `tag = 'A' + (w>>>27)`,
`custom = (w>>>26)&1`, `side = (w>>>24)&3`, `end = w & 0xFFFFFF`; a token's
`start` is the previous token's `end`.

##  script args

```js
args;          // the argv tail after the script path: jab t.js a b → ["a","b"]
process.argv;  // Node-shaped: ["jab", <script path>, ...args]
```

Both globals are installed before the script runs; under `--eval` (no script
file) `args` is empty. They are plain JS-owned strings — the binding keeps no
reference after bootstrap.  JAB-001: `jab` takes either an explicit path
(`/`,`./`,`../` — run directly) or a bareword (resolved via the upward `jsrc/`-scan,
with `process.argv[1]` patched to the resolved abspath).

##  Examples

```js
// stream-copy src → dst, zero offset arithmetic, one reusable buffer
let b = io.buf(1 << 16);
while (io.read(src, b) > 0)      // fill DATA from src
  while (b.size) io.write(dst, b); // drain DATA to dst
```

```js
// build a record and write it once
let b = io.buf(256);
b.feedStr("GET "); b.feed(path); b.feed1(0x0a);
io.writeAll(sock, b);
```

```js
// scan a mmapped file with no I/O and no copies
let m = io.mmap("index.u64", "r");
let view = m.data();             // Uint8Array straight over page-cache
// ... read view[...] ; bytes fault in lazily; GC munmaps when m dies
```

```js
// append to a file while keeping views valid
let k = io.book("log", 1 << 30, 4096);
k.feedStr("line\n");             // grows the mapping under the hood
if (k.room < 64) k.grow(k.cap + 4096); // stable base — k.data() stays valid
k.msync();
```

##  ABC mapping

| JS                        | ABC (`abc/Bx.h`, `abc/FILE.h`)        |
|---------------------------|----------------------------------------|
| `Buf` regions             | `u8b` `PAST \| DATA \| IDLE`            |
| `b.feed` / `feed1`        | `u8bFeed` / `u8bFeed1`                  |
| `b.feedStr`               | `utf8` encode + `u8bFed`               |
| `b.take` / `skip`         | `u8bUsed`                              |
| `b.shed` / `pop` / `reset`| `u8bShed` / `u8bPop` / `u8bReset`      |
| `b.shift` / `splice`      | `u8bShift` / `u8bSplice`               |
| `b.grow` (heap/ram)       | `u8bReMap`                             |
| `b.grow` (book)           | `FILEBookExtend`                       |
| `io.read*`                | `FILEDrain` / `FILEdrainall` / `FILEdrainv` |
| `io.write*`               | `FILEFeed` / `FILEFeedAll` / `FILEFeedv`    |
| `io.mmap` `r`/`rw`/`c`    | `FILEMapRO` / `FILEMapRW` / `FILEMapCreate`  |
| `io.spawn` / `spawnFds`   | `FILESpawn` / `FILESpawnFds`           |
| `io.reap`                 | `FILEReap` (`{code}` / `{signal}`)     |
| `io.unlink`               | `FILEUnLink`                           |
| `io.rename` / `io.mkdir`  | `FILERename` / `FILEMakeDirP`          |
| `io.rmdir`                | `FILERmDir` (`recursive` = rm -rf)     |
| `io.stat` / `io.lstat`    | `FILEStat` / `FILELStat` (`mtime`/`atime` ron60 BigInt) |
| `io.readlink` / `symlink` | `FILEReadLink` / `FILESymLink`         |
| `io.chmod`                | `FILEChmod`                            |
| `io.setMtime`             | `FILESetMtime` (`utimensat` NOFOLLOW)  |
| `abc.index` get/range     | `<lane>sFindGE` / `HIT<lane>SeekRange` |
| `abc.index` compact       | `HIT<lane>Compact` (1/8 ladder)        |
| `io.book`                 | `FILEBookCreate`                       |
| `b.msync` / `trim`        | `FILEMSync` / `FILETrimMap`            |
| `io.ram`                  | `u8bMap` (anonymous)                   |
| `ron.now` / `of`          | `RONNow` / `localtime`+`RONOfTime`     |
| `ron.date`                | `RONToTime`+`mktime`+`DOGutf8sFeedDate`|
| `zip.deflate` / `inflate` | `ZINFDeflate` / `ZINFInflate`          |
