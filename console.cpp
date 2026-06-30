#include "JABC.hpp"

//  JAB-002: a standard `console` — pure JS (like ansi.cpp / buf.cpp), layering
//  over the existing leaves, no native encoding or raw IO of its own.  Output
//  goes through `utf8.Encode` + `io.writeAll` to a chosen fd: log/info/debug →
//  stdout (1), warn/error/trace/assert → stderr (2), like Node.  `format` does
//  the multi-arg space-join + `%s %d %i %f %j %o %O %c %%` expansion; `inspect`
//  is a small JSON-ish stringifier with a cycle guard (`[Circular]`).
static const char* JABC_CONSOLE_JS = R"JS(
(function (g) {
  "use strict";
  const io = g.io, utf8 = g.utf8;

  //  A small util.inspect: scalars stringified, objects/arrays rendered
  //  JSON-ish (single-quoted strings, bareword keys), cycles → [Circular].
  function inspect(v, seen) {
    const t = typeof v;
    if (t === "string") return "'" + v.replace(/\\/g, "\\\\")
        .replace(/'/g, "\\'").replace(/\n/g, "\\n") + "'";
    if (t === "bigint") return String(v) + "n";
    if (t === "function") return "[Function" + (v.name ? ": " + v.name : " (anonymous)") + "]";
    if (t === "symbol") return v.toString();
    if (t !== "object" || v === null) return String(v);  // number/boolean/undefined/null
    seen = seen || [];
    if (seen.indexOf(v) >= 0) return "[Circular]";
    if (v instanceof Error) return v.stack || (v.name + ": " + v.message);
    seen.push(v);
    let out;
    if (Array.isArray(v)) {
      out = v.length ? "[ " + v.map((x) => inspect(x, seen)).join(", ") + " ]" : "[]";
    } else {
      const keys = Object.keys(v);
      const body = keys.map((k) => {
        const key = /^[A-Za-z_$][\w$]*$/.test(k) ? k : "'" + k + "'";
        return key + ": " + inspect(v[k], seen);
      });
      out = body.length ? "{ " + body.join(", ") + " }" : "{}";
    }
    seen.pop();
    return out;
  }

  //  Stringify one non-specifier arg: a top-level string prints raw (no
  //  quotes), everything else goes through inspect.
  function str(a) { return typeof a === "string" ? a : inspect(a); }

  //  Node-style util.format over an arguments/array `a`.
  function format(a) {
    if (a.length === 0) return "";
    let i = 1, out;
    const first = a[0];
    if (typeof first === "string" && /%[sdifjoOc%]/.test(first)) {
      out = first.replace(/%([sdifjoOc%])/g, (m, k) => {
        if (k === "%") return "%";
        if (i >= a.length) return m;          // not enough args: leave literal
        const x = a[i++];
        switch (k) {
          case "s": return typeof x === "object" && x !== null ? inspect(x)
              : (typeof x === "bigint" ? String(x) + "n" : String(x));
          case "d": case "i": {
            if (typeof x === "bigint") return String(x) + "n";
            const n = Number(x); return Number.isNaN(n) ? "NaN" : String(Math.trunc(n));
          }
          case "f": { const n = Number(x); return Number.isNaN(n) ? "NaN" : String(n); }
          case "j": try { return JSON.stringify(x); } catch (e) { return "[Circular]"; }
          case "o": case "O": return inspect(x);
          case "c": return "";                // CSS directive: consumed, no output
        }
      });
    } else {
      out = str(first);
    }
    for (; i < a.length; i++) out += " " + str(a[i]);
    return out;
  }

  //  The one write path: format → utf8 bytes → io.writeAll to the fd (reusing
  //  the buf.cpp leaves; routing through io.writeAll keeps it interceptable).
  function emit(fd, s) { io.writeAll(fd, utf8.Encode(s + "\n")); }

  g.console = {
    log:   function () { emit(io.stdout, format(arguments)); },
    info:  function () { emit(io.stdout, format(arguments)); },
    debug: function () { emit(io.stdout, format(arguments)); },
    warn:  function () { emit(io.stderr, format(arguments)); },
    error: function () { emit(io.stderr, format(arguments)); },
    trace: function () { emit(io.stderr, "Trace: " + format(arguments)); },
    dir:   function (obj) { emit(io.stdout, inspect(obj)); },
    assert: function (cond) {
      if (cond) return;
      const rest = Array.prototype.slice.call(arguments, 1);
      emit(io.stderr, rest.length ? "Assertion failed: " + format(rest) : "Assertion failed");
    },
  };
})(this);
)JS";

ok64 JABCConsoleInstall() {
  JABCExecute(JABC_CONSOLE_JS);
  return OK;
}
