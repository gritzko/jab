"use strict";
// JAB-035: the toy pack's entry — a stand-in for beagle's jsrc/main.js, so a
// packed test binary has the one file every jsrc tree must have.
const util = require("util.js");
io.log("packtoy main: " + util.name + "/" + require("lib/dep.js").name);
