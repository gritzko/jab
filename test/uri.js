"use strict";
// URI class over abc/URI: parse into 8 components, compose, escape.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": '" + a + "' !== '" + b + "'"); }

// full authority URI
{
  const u = new URI("https://user@host:443/a/b?x=1#frag");
  eq(u.scheme, "https", "scheme");
  eq(u.authority, "//user@host:443", "authority");
  eq(u.user, "user", "user");
  eq(u.host, "host", "host");
  eq(u.port, "443", "port");
  eq(u.path, "/a/b", "path");
  eq(u.query, "x=1", "query");
  eq(u.fragment, "frag", "fragment");
  eq(u.toString(), "https://user@host:443/a/b?x=1#frag", "toString");
}

// beagle-shape URI (no authority): scheme:path?branch#msg
// absent component -> undefined (not ""); see "/p" vs "/p?" below
{
  const b = new URI("file:/home/x/.be?/dogs/main#msg");
  eq(b.scheme, "file", "be scheme");
  eq(b.authority, undefined, "be authority (absent)");
  eq(b.host, undefined, "be host (absent)");
  eq(b.path, "/home/x/.be", "be path");
  eq(b.query, "/dogs/main", "be query");
  eq(b.fragment, "msg", "be fragment");
}

// absent vs present-but-empty: "/p" has no query, "/p?" has empty query
{
  const a = new URI("/p");
  eq(a.path, "/p", "no-query path");
  eq(a.query, undefined, "absent query -> undefined");
  eq(a.fragment, undefined, "absent fragment -> undefined");
  const e = new URI("/p?");
  eq(e.path, "/p", "empty-query path");
  eq(e.query, "", "present-empty query -> ''");
  const f = new URI("/p#");
  eq(f.fragment, "", "present-empty fragment -> ''");
}

// compose + percent-escape
eq(URI.make("http", "ex.com", "/p", "q=1", "f"), "http:ex.com/p?q=1#f", "make");
eq(URI.unescape(URI.escape("a b/c")), "a b/c", "escape roundtrip");

// URI-009 (make side): absent (undefined) omits the sigil; present-empty
// ("") emits a bare `?`/`#` — make is the exact inverse of _parse.
{
  eq(URI.make(undefined, undefined, "dog/DOG.h", undefined, undefined),
     "dog/DOG.h", "absent q+f -> no ?#");
  eq(URI.make(undefined, undefined, "dog/DOG.h", undefined, "L5"),
     "dog/DOG.h#L5", "absent query + present frag -> #L5, no ?");
  eq(URI.make(undefined, undefined, "dog/DOG.h", "", undefined),
     "dog/DOG.h?", "present-empty query -> bare ?");
  eq(URI.make(undefined, undefined, undefined, "b", ""), "?b#",
     "present-empty fragment -> trailing #");
}

// parse -> make round-trip: every shape composes back byte-identical.
{
  const rts = ["dog/DOG.h", "dog/DOG.h?main", "dog/DOG.h#L5",
               "dog/DOG.h?main#L5", "//DIS-060/dog?main", "?feat", "#L5",
               "a?", "a#", "a?#b", "file:/home/x/.be?/dogs/main#msg"];
  for (const s of rts) {
    const u = new URI(s);
    eq(URI.make(u.scheme, u.authority, u.path, u.query, u.fragment), s,
       "round-trip " + s);
  }
}

// JS-108: slice->string conversion is length-exact — a >2047-byte escape
// result must come back whole (was: silent clamp in uri.cpp JABCSliceStr).
{
  const sp = " ".repeat(1200);              // escapes to %20 x 1200 = 3600
  const esc = URI.escape(sp);
  eq(esc.length, 3600, "long escape length");
  if (esc !== "%20".repeat(1200)) fail("long escape content");
}

io.log("uri.js OK");
