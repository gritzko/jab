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
  //  JAB-001: a bareword (no /, ./, ../) is NOT path-relative — it resolves
  //  by scanning UP for `./be ../be …`, trying <be>/<name> then <name>.js in
  //  each.  Ceiling: $HOME/be (under $HOME) else /be.  The scan starts at
  //  `from` — the REQUIRING script's dir for a require() (so `require("lib/
  //  x.js")` finds the be/ shard nearest the script, cwd-independent and free
  //  of the cwd wrong-copy hazard); cwd for a bareword ENTRY (no requirer).
  function isExplicit(spec) {
    return spec[0] === "/" || spec.slice(0, 2) === "./" ||
           spec.slice(0, 3) === "../";
  }
  function resolveBe(spec, from) {
    const start = from || io.cwd();
    const home = io.getenv("HOME");
    const ceil = home ? normalize(home) : "/";
    let dir = normalize(start);
    while (true) {
      const be = dir + "/be";
      for (const c of [be + "/" + spec, be + "/" + spec + ".js"])
        if (isFile(c)) return c;
      if (dir === ceil || dir === "/") break;
      dir = dirname(dir);
    }
    throw "require: cannot find 'be/" + spec + "' from '" + start + "'";
  }
  function resolve(spec, baseDir) {
    if (!isExplicit(spec)) return resolveBe(spec, baseDir);
    let base = normalize(spec[0] === "/" ? spec : baseDir + "/" + spec);
    for (const c of [base, base + ".js", base + "/index.js"])
      if (isFile(c)) return c;
    throw "require: cannot find '" + spec + "' from '" + baseDir + "'";
  }
  function makeRequire(baseDir) {
    const req = (spec) => load(spec, baseDir);
    req.resolve = (spec) => resolve(spec, baseDir);
    req.cache = cache;
    return req;
  }
  function load(spec, baseDir) {
    const abs = resolve(spec, baseDir);
    if (cache[abs]) return cache[abs].exports;
    const src = utf8.Decode(io.mmap(abs, "r").data());
    const module = { exports: {}, id: abs, filename: abs };
    cache[abs] = module;                       // before eval: cycle-safe
    const dir = dirname(abs);
    const fn = new Function("module", "exports", "require",
                            "__filename", "__dirname", src);
    fn(module, module.exports, makeRequire(dir), abs, dir);
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

  //  JAB-001: `jab <bareword>` entry.  main.cpp sets g.__mainSpec then calls
  //  __main(): resolve the bareword via the be/-scan, patch process.argv[1] to
  //  the resolved abspath (so the script's `here` idiom sees its real path),
  //  then load it through require (cache + relative ./ resolution apply).
  g.__main = function (spec) {
    const abs = resolveBe(spec);
    if (g.process && g.process.argv) g.process.argv[1] = abs;
    return load(abs, dirname(abs));
  };
})(this);
)JS";

ok64 JABCRequireInstall() {
  JABCExecute(JABC_REQUIRE_JS);
  return OK;
}
