"use strict";
// JAB-035: the pack FORMAT — jsrcpack.js (the build-time packer cmake runs)
// must emit the 16-byte preamble ("JSR", version, raw length, content hash)
// over a deflated ustar whose bytes are DETERMINISTIC: the same tree packs to
// the same bytes, hence the same cache dir, hence a reused extraction.
function fail(m) { throw "FAIL " + m; }
function eq(a, b, m) { if (a !== b) fail(m + ": " + a + " !== " + b); }

const packer = require("../jsrcpack.js");

// fixture: a small jsrc-shaped tree (nested dir, an empty file, a dotfile that
// the walk must skip).
const root = (io.getenv("TMPDIR") || "/tmp") + "/jab_jsrcpack_" + io.getpid();
try { io.rmdir(root, true); } catch (e) { /* no leftover from a crashed run */ }
//  A >100-char path exercises the ustar prefix[155] split.
const LONG = "lib/a_directory_name_long_enough_to_push_this_entry/" +
             "past_the_hundred_char_ustar_name_field/deep.js";
io.mkdir(root + "/" + LONG.slice(0, LONG.lastIndexOf("/")));
const files = {
  "main.js": "module.exports = 1;\n",
  "util.js": "module.exports = 2;\n",
  "lib/dep.js": "module.exports = 3;\n",
  "empty.js": "",
};
files[LONG] = "module.exports = 4;\n";
function write(p, s) {
  const fd = io.open(p, "c");
  try { io.writeAll(fd, utf8.Encode(s)); } finally { io.close(fd); }
}
for (const n in files) write(root + "/" + n, files[n]);
write(root + "/.hidden", "not packed\n");

const pack = packer.pack(root);
const again = packer.pack(root + "/");        // a trailing slash changes nothing
eq(pack.length, again.length, "pack length differs per run");
for (let i = 0; i < pack.length; i++)
  if (pack[i] !== again[i]) fail("pack bytes are not deterministic at " + i);

// --- preamble ---------------------------------------------------------------
eq(utf8.Decode(pack.subarray(0, 3)), "JSR", "magic");
eq(pack[3], packer.VERSION, "format version");
const rawlen = pack[4] + pack[5] * 256 + pack[6] * 65536 + pack[7] * 16777216;
const raw = new Uint8Array(rawlen);
eq(zip._inflate(pack.subarray(packer.PRE), raw, 0), rawlen,
   "inflated length vs the preamble's");
const hash = sha256(raw);
for (let i = 0; i < 8; i++)
  eq(pack[8 + i], hash[i], "content hash byte " + i);

// --- the ustar --------------------------------------------------------------
function str(b, off, len) {
  let end = off;
  while (end < off + len && b[end] !== 0) end++;
  return utf8.Decode(b.subarray(off, end));
}
const got = {};
let off = 0, prev = "";
while (off + 512 <= raw.length && raw[off] !== 0) {
  const prefix = str(raw, off + 345, 155);
  const name = prefix ? prefix + "/" + str(raw, off, 100) : str(raw, off, 100);
  if ((name.length > 100) !== (prefix !== ""))
    fail("prefix split: " + name);
  const size = parseInt(str(raw, off + 124, 12).trim() || "0", 8);
  eq(str(raw, off + 257, 6), "ustar", name + ": ustar magic");
  eq(str(raw, off + 136, 12).trim(), "00000000000", name + ": mtime zeroed");
  eq(str(raw, off + 108, 8).trim(), "0000000", name + ": uid zeroed");
  eq(raw[off + 156], 48, name + ": regular-file typeflag");
  if (name < prev) fail("entries are not sorted: " + name + " after " + prev);
  prev = name;
  let sum = 0;                                 // chksum: the field reads as spaces
  for (let i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? 32 : raw[off + i];
  eq(parseInt(str(raw, off + 148, 8).trim(), 8), sum, name + ": header checksum");
  off += 512;
  got[name] = utf8.Decode(raw.subarray(off, off + size));
  off += (size + 511) & ~511;
}
for (const n in files) eq(got[n], files[n], "unpacked " + n);
eq(Object.keys(got).length, Object.keys(files).length,
   "entry count (a dotfile must not be packed)");

io.rmdir(root, true);
io.log("jsrcpack.js OK");
