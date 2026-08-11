#include "JABC.hpp"

//  Synchronous CommonJS require() — built entirely on the existing bindings
//  (io.mmap to read the source, utf8.Decode, io.stat to probe).  No engine
//  module loader, no promises: resolve -> mmap -> wrap -> eval -> cache.
//  Each module gets its own require bound to its directory (relative
//  resolution); the cache is keyed by resolved absolute path and the module
//  is inserted BEFORE eval so cycles see a partial exports (CommonJS rule).
static const char* JABC_REQUIRE_JS = R"JS(
(function (g) {
  "use strict";
  const io = g.io, utf8 = g.utf8;
  const cache = {};

  function normalize(p) {
    const abs = p[0] === "/";
    const out = [];
    for (const s of p.split("/")) {
      if (s === "" || s === ".") continue;
      if (s === "..") {
        if (out.length && out[out.length - 1] !== "..") out.pop();
        else if (!abs) out.push("..");
      } else out.push(s);
    }
    return (abs ? "/" : "") + out.join("/");
  }
  function dirname(p) {
    const i = p.lastIndexOf("/");
    return i < 0 ? "." : (i === 0 ? "/" : p.slice(0, i));
  }
  function isFile(p) {
    try { return io.stat(p).kind === "reg"; } catch (e) { return false; }
  }
  function isDir(p) {
    try { return io.stat(p).kind === "dir"; } catch (e) { return false; }
  }
  //  JAB-001: a bareword (no /, ./, ../) is NOT path-relative — it resolves
  //  by scanning UP for `./jsrc ../jsrc …`, trying <jsrc>/<name> then
  //  <name>.js in each.  Ceiling: $HOME/jsrc (under $HOME) else /jsrc.
  //  GET-041: the jsrc-CLIMB runs ONCE — at the first lookup (startup) — and
  //  its result is FROZEN as an array of absolute jsrc/ dir paths (symlinks
  //  kept verbatim: a `$HOME/jsrc -> …` locator stays a locator).  Every bareword
  //  then resolves against that fixed array, so a `jab get` re-scanning per
  //  require can never wander into a tree it is mid-writing.  Explicit ./ ../
  //  paths never touch the array — resolve() keeps them relative to the
  //  requiring script's own dir.
  function isExplicit(spec) {
    return spec[0] === "/" || spec.slice(0, 2) === "./" ||
           spec.slice(0, 3) === "../";
  }
  //  JAB-035: the FLOOR — the jsrc pack embedded in the binary (the global
  //  `jsrcpack`, absent without -DJAB_JSRC), extracted once and appended LAST.
  let packErr = null;
  function packHex(b) {
    let s = "";
    for (let i = 8; i < 16; i++) s += (b[i] < 16 ? "0" : "") + b[i].toString(16);
    return s;
  }
  function packStr(b, off, len) {
    let end = off;
    while (end < off + len && b[end] !== 0) end++;
    return utf8.Decode(b.subarray(off, end));
  }
  //  Untar the inflated pack into `dir`: a 512-byte header per entry, then the
  //  file bytes padded up to the next 512 boundary; a zero header ends it.
  function packUntar(dir, raw) {
    let off = 0;
    while (off + 512 <= raw.length && raw[off] !== 0) {
      let name = packStr(raw, off, 100);
      const prefix = packStr(raw, off + 345, 155);
      if (prefix) name = prefix + "/" + name;
      const size = parseInt(packStr(raw, off + 124, 12).trim() || "0", 8);
      const type = raw[off + 156];
      off += 512;
      if (type === 48 || type === 0) {          // '0' / NUL: a regular file
        const p = dir + "/" + name;
        io.mkdir(dirname(p));
        const fd = io.open(p, "c");
        try { io.writeAll(fd, raw.subarray(off, off + size)); }
        finally { io.close(fd); }
      }
      off += (size + 511) & ~511;
    }
  }
  //  Cache probe, then inflate+untar into a temp dir and rename it into place;
  //  a lost race (the dir is already there) is a win — drop ours, use theirs.
  function packExtract() {
    const b = g.jsrcpack;
    if (!b || b.length < 16 || b[0] !== 74 || b[1] !== 83 || b[2] !== 82)
      return null;                              // no "JSR" pack: no floor
    if (b[3] !== 1)
      throw "the built-in scripts are packed in a format this jab does not know";
    const home = io.getenv("HOME");
    const cache = io.getenv("XDG_CACHE_HOME") || (home ? home + "/.cache" : "");
    if (!cache)
      throw "neither XDG_CACHE_HOME nor HOME is set, so there is nowhere to " +
            "unpack the built-in scripts";
    const dir = cache + "/jsrcs/" + packHex(b);
    if (isDir(dir)) return dir;
    const raw = new Uint8Array(b[4] + b[5] * 256 + b[6] * 65536 + b[7] * 16777216);
    g.zip._inflate(b.subarray(16), raw, 0);     // the preamble pre-sizes it
    const tmp = cache + "/jsrcs/.tmp-" + io.getpid();
    io.mkdir(tmp);
    try {
      packUntar(tmp, raw);
      io.rename(tmp, dir);
    } catch (e) {
      io.rmdir(tmp, true);
      if (!isDir(dir)) throw e;
    }
    return dir;
  }

  let jsrcStack = null;                        // frozen once, at startup
  function pinJsrcStack(start) {
    const abs = normalize(start[0] === "/" ? start : io.cwd() + "/" + start);
    const home = io.getenv("HOME");
    const ceil = home ? normalize(home) : "/";
    let dir = abs;
    const out = [];
    while (true) {
      const jsrc = dir + "/jsrc";
      if (isDir(jsrc)) out.push(jsrc);
      if (dir === ceil || dir === "/") break;
      dir = dirname(dir);
    }
    //  JAB-035: the pack floor joins the one-time pin, BELOW every real jsrc.
    try { const floor = packExtract(); if (floor) out.push(floor); }
    catch (e) { packErr = e; }
    jsrcStack = out;
    return out;
  }
  function resolveJsrc(spec, from) {
    const stack = jsrcStack || pinJsrcStack(from || io.cwd());
    for (const jsrc of stack)
      for (const c of [jsrc + "/" + spec, jsrc + "/" + spec + ".js"])
        if (isFile(c)) return c;
    throw "require: cannot find 'jsrc/" + spec + "' from '" +
          (from || io.cwd()) + "'" + (packErr ?
          " (jab could not unpack its built-in scripts: " + packErr + ")" : "");
  }
  function resolve(spec, baseDir) {
    if (!isExplicit(spec)) return resolveJsrc(spec, baseDir);
    let base = normalize(spec[0] === "/" ? spec : baseDir + "/" + spec);
    for (const c of [base, base + ".js", base + "/index.js"])
      if (isFile(c)) return c;
    throw "require: cannot find '" + spec + "' from '" + baseDir + "'";
  }
  //  JAB-010: memo (baseDir, spec) -> abs, consulted BEFORE resolve(), which
  //  re-ran its io.stat ladder on every require — even a cache-hit one.
  const resolved = new Map();
  function resolveMemo(spec, baseDir) {
    //  Skip a cwd-dependent pair (relative spec off a relative base): only
    //  those can change under io.chdir, so today's semantics stay (JS-119).
    if (isExplicit(spec) && spec[0] !== "/" && baseDir[0] !== "/")
      return resolve(spec, baseDir);
    const key = spec.length + ":" + spec + baseDir;   // unambiguous
    let abs = resolved.get(key);
    if (abs === undefined) {
      abs = resolve(spec, baseDir);            // throws: nothing memoized
      resolved.set(key, abs);
    }
    return abs;
  }
  function makeRequire(baseDir) {
    const req = (spec) => load(spec, baseDir);
    req.resolve = (spec) => resolveMemo(spec, baseDir);
    req.cache = cache;
    return req;
  }
  function load(spec, baseDir) {
    const abs = resolveMemo(spec, baseDir);
    if (cache[abs]) return cache[abs].exports;
    const src = utf8.Decode(io.mmap(abs, "r").data());
    const module = { exports: {}, id: abs, filename: abs };
    cache[abs] = module;                       // before eval: cycle-safe
    const dir = dirname(abs);
    //  JS-112: compile/eval failure evicts the cache entry and rethrows so a
    //  retry re-evaluates (Node CommonJS); before-eval insert stays for cycles.
    try {
      const fn = new Function("module", "exports", "require",
                              "__filename", "__dirname", src);
      fn(module, module.exports, makeRequire(dir), abs, dir);
    } catch (e) { delete cache[abs]; throw e; }
    return module.exports;
  }

  g.require = makeRequire(".");                 // top-level: resolve vs cwd

  //  An EXPLICIT-path entry (`jab ./x.js`, `jab /abs/x.js`) runs via global
  //  eval, so its top-level require would resolve `./lib/y.js` against the
  //  CWD, not the script.  Rebind it to the script's OWN dir so a sibling
  //  `require("./lib/y.js")` works script-relative — no `here` idiom needed.
  //  main.cpp calls this before eval'ing the file; bareword entries skip it
  //  (they load through __main, which already binds require to the module dir).
  g.__rebaseRequire = function (p) {
    const abs = normalize(p[0] === "/" ? p : io.cwd() + "/" + p);
    g.require = makeRequire(dirname(abs));
  };

  //  JAB: a bare/relative `.js` ENTRY (`jab foo.js`, `jab jsrc/main.js`) — NOT
  //  an explicit path (main.cpp runs /,./,../ directly via global eval).  Resolve
  //  it via the upward jsrc/-scan, pin it as argv[1], and load it as the program.
  g.__runScript = function (spec) {
    const abs = resolveJsrc(spec);
    if (g.process && g.process.argv) g.process.argv[1] = abs;
    return load(abs, dirname(abs));
  };

  //  JAB: the DEFAULT entry — any first arg that is NOT a `.js` file (a verb, a
  //  `scheme:` URI, a non-.js path) and the no-arg `jab` route here (main.cpp
  //  decides via the `.js` suffix and calls __main()).  Resolve jsrc/main.js by
  //  the same upward jsrc/-scan, splice it in as argv[1] (so main.js's
  //  process.argv[1] is its OWN jsrc/-root path — core/loop.js derives `_here`
  //  from it — and the user's tokens stay at argv[2:] AS-IS, the cli contract:
  //  verb=argv[2]), then run it; main.js (the resident loop) triages the rest.
  g.__main = function () {
    const abs = resolveJsrc("main.js");
    const av = g.process && g.process.argv;
    if (av) g.process.argv = [av[0], abs].concat(av.slice(1));
    return load(abs, dirname(abs));
  };
})(this);
)JS";

ok64 JABCRequireInstall() {
  JABCExecute(JABC_REQUIRE_JS);
  return OK;
}
