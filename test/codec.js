"use strict";
// hex encode/decode/encodeInto + sha1/sha256 (known test vectors).
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

// hex roundtrip
{
  const b = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
  eq(hex.encode(b), "deadbeef", "hex encode");
  const back = hex.decode("deadbeef");
  eq(back.length, 4, "hex decode len");
  eq(hex.encode(back), "deadbeef", "hex roundtrip");
  eq(hex.encode(new Uint8Array(0)), "", "hex empty");
}

// hex.encodeInto (provided buffer)
{
  const dst = new Uint8Array(8);
  const n = hex.encodeInto(new Uint8Array([0xde, 0xad, 0xbe, 0xef]), dst);
  eq(n, 8, "encodeInto n");
  eq(utf8.Decode(dst.subarray(0, n)), "deadbeef", "encodeInto content");
}

// bad hex rejected
{
  let threw = false;
  try { hex.decode("xyz"); } catch (e) { threw = true; }
  if (!threw) fail("bad hex not rejected");
}

// sha1/sha256 known vectors for "abc"
{
  const abc = utf8.Encode("abc");
  const d1 = sha1(abc);
  eq(d1.length, 20, "sha1 len");
  eq(hex.encode(d1), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(abc)");
  const d2 = sha256(abc);
  eq(d2.length, 32, "sha256 len");
  eq(hex.encode(d2),
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
     "sha256(abc)");
  // empty-input vector
  eq(hex.encode(sha1(new Uint8Array(0))),
     "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1(empty)");
}

// ron60 codec round-trips (verbs, and a timestamp-width string)
{
  for (const s of ["get", "post", "delete", "26617GKE4b"])
    eq(ron.encode(ron.decode(s)), s, "ron roundtrip " + s);
}

// JS-021 ron time codec: now()/of()/date() over RONNow/RONOfTime/DOGutf8sFeedDate.
{
  if (typeof ron.now() !== "bigint") fail("ron.now not bigint");
  // of(Date) === of(ms-int): same instant, two arg shapes.
  // 1700000000000 ms = 2023-11-14, inside ron60's 2000-2099 YY range.
  eq(ron.of(new Date(1700000000000)), ron.of(1700000000000), "ron.of Date vs ms");
  // date() is a non-empty (7-col centred) string; date(0n) is the "?" placeholder.
  const d = ron.date(ron.now());
  if (typeof d !== "string" || d.trim().length === 0) fail("ron.date empty");
  eq(ron.date(0n), "   ?   ", "ron.date(0) placeholder");
  // of(now-ms) -> date() lands in the HH:MM bucket (same minute, <12h).
  const hhmm = ron.date(ron.of(Date.now())).trim();
  if (hhmm.length !== 5 || hhmm[2] !== ":") fail("ron.date HH:MM: " + hhmm);
}

io.log("codec.js OK");
