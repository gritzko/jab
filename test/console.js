"use strict";
// JAB-002: the `console` object — log/info/debug → stdout (fd 1), warn/error →
// stderr (fd 2), Node-style multi-arg + %s/%d/%i/%f/%j/%o/%% formatting.  We
// intercept the io.writeAll leaf console layers over (buf.cpp) to capture both
// the fd and the exact formatted bytes in-process — so one .js test asserts
// routing AND formatting.  Each assertion throws -> non-zero exit (CTest fails).
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + JSON.stringify(a) + " !== " + JSON.stringify(b)); }

let cap;
const real = io.writeAll;
io.writeAll = (fd, bytes) => { cap.push([fd, utf8.Decode(bytes)]); return bytes.length; };
function grab(fn) { cap = []; fn(); return cap; }
function one(fn) { const c = grab(fn); if (c.length !== 1) fail("expected 1 write, got " + c.length); return c[0]; }

try {
  // fd routing: log/info/debug -> stdout (1), warn/error -> stderr (2)
  eq(one(() => console.log("x"))[0], 1, "log->stdout");
  eq(one(() => console.info("x"))[0], 1, "info->stdout");
  eq(one(() => console.debug("x"))[0], 1, "debug->stdout");
  eq(one(() => console.warn("x"))[0], 2, "warn->stderr");
  eq(one(() => console.error("x"))[0], 2, "error->stderr");

  // trailing newline + plain text
  eq(one(() => console.log("hi"))[1], "hi\n", "newline");

  // multi-arg space join
  eq(one(() => console.log("a", "b", "c"))[1], "a b c\n", "multi-arg join");

  // printf specifiers
  eq(one(() => console.log("%s/%d", "x", 5))[1], "x/5\n", "%s %d");
  eq(one(() => console.log("%i", 3.9))[1], "3\n", "%i truncates");
  eq(one(() => console.log("%f", 3.5))[1], "3.5\n", "%f");
  eq(one(() => console.log("100%%"))[1], "100%\n", "%% literal");
  eq(one(() => console.log("50% off"))[1], "50% off\n", "bare % is literal");

  // extra args beyond specifiers are space-appended; missing args stay literal
  eq(one(() => console.log("%s", "a", "b"))[1], "a b\n", "extra args appended");
  eq(one(() => console.log("%s %s", "a"))[1], "a %s\n", "missing arg literal");

  // object / array inspection (%o and bare)
  eq(one(() => console.log("%o", { a: 1 }))[1], "{ a: 1 }\n", "%o object");
  eq(one(() => console.log({ a: 1, b: "c" }))[1], "{ a: 1, b: 'c' }\n", "bare object");
  eq(one(() => console.log([1, 2, 3]))[1], "[ 1, 2, 3 ]\n", "array");
  eq(one(() => console.log({}))[1], "{}\n", "empty object");

  // %j is JSON; a cycle degrades to [Circular] (no throw)
  eq(one(() => console.log("%j", { a: 1 }))[1], '{"a":1}\n', "%j json");
  const cyc = {}; cyc.self = cyc;
  eq(one(() => console.log("%j", cyc))[1], "[Circular]\n", "%j cycle");
  if (one(() => console.log(cyc))[1].indexOf("Circular") < 0) fail("bare cycle not handled");

  // non-string scalars stringified
  eq(one(() => console.log(42))[1], "42\n", "number");
  eq(one(() => console.log(true, null))[1], "true null\n", "bool+null");
  if (one(() => console.log(function foo() {}))[1].indexOf("Function") < 0) fail("function not inspected");

  // assert: truthy is silent, falsy writes "Assertion failed" to stderr
  eq(grab(() => console.assert(true, "nope")).length, 0, "assert(true) silent");
  const a = one(() => console.assert(false, "boom"));
  eq(a[0], 2, "assert->stderr");
  if (a[1].indexOf("Assertion failed") < 0) fail("assert text: " + a[1]);
} finally {
  io.writeAll = real;
}

io.log("console.js OK");
