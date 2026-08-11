cmake_minimum_required(VERSION 3.20)
project(jabc C CXX)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-strict-aliasing")
set(CMAKE_EXPORT_COMPILE_COMMANDS on)
set(DOG_BIN_DIR ${CMAKE_BINARY_DIR}/bin)
include(CTest)

#  curl is a hard dependency of the build: the full `abc` (which jabc_test
#  links) carries CURL.c.  Checked before descending into dog/abc so a
#  missing libcurl-dev is named on the first lines of configure output
#  instead of surfacing as `cannot find -labc` at link time.
find_package(CURL REQUIRED)

#  JAB-005: abc is NESTED in the dog submodule; dog's guarded
#  add_subdirectory(abc) provides abc-core, `-Idog` resolves "abc/...".
set(ABC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dog/abc)
if (NOT TARGET dog)
    add_subdirectory(dog)
endif()

if(APPLE)
    find_library(JSC_LIB JavaScriptCore)
    set(JSC_INCLUDE_DIR "")
else()
    find_library(JSC_LIB
        NAMES javascriptcoregtk-4.1 javascriptcoregtk-4.0
        PATHS /usr/lib/x86_64-linux-gnu/ /usr/lib
    )
    find_path(JSC_INCLUDE_DIR
        NAMES JavaScriptCore/JSBase.h
        PATH_SUFFIXES webkitgtk-4.1/ webkitgtk-4.0/
    )
endif()

#  JavaScriptCore is mandatory: jab IS the JS runtime, so without it there is
#  no jab target to build.  The old WARNING + return() ended configure
#  "successfully" with the `jab` binary silently absent from the build.
#  Ubuntu/Debian: libjavascriptcoregtk-4.1-dev.  macOS: system framework.
if(NOT JSC_LIB)
    message(FATAL_ERROR
        "JavaScriptCore not found — install libjavascriptcoregtk-4.1-dev "
        "(Debian/Ubuntu) or set JSC_LIB")
endif()
if(NOT APPLE AND NOT JSC_INCLUDE_DIR)
    message(FATAL_ERROR
        "JavaScriptCore headers (JavaScriptCore/JSBase.h) not found — install "
        "libjavascriptcoregtk-4.1-dev (Debian/Ubuntu) or set JSC_INCLUDE_DIR")
endif()

# SODIUM_INCLUDE_DIR is cached by dog/abc's find_path; jab TUs reach sodium
# headers via ABC.h -> NACL.h, and /opt/homebrew/include is not a default
# search path on arm64 macOS.
include_directories(
    ${JSC_INCLUDE_DIR}
    ${SODIUM_INCLUDE_DIR}
    .
    dog
)

# The JS Buf class is embedded in buf.cpp as a raw string literal; no js2c /
# node codegen step (node is not a build dependency).
# abc/POL.c (pol) and NET/TCP/UDP.c (net/dgram) are stripped from abc-core
# (grouped with CURL); compile them straight into jab for the socket/loop leaves.
# JAB-031: abc/FSW.c is in no abc library either (it had zero consumers) —
# same treatment as POL.c: compiled straight into jab for the fsw leaves.
set(JABC_SRC main.cpp arg.cpp io.cpp utf8.cpp buf.cpp console.cpp cont.cpp tok.cpp uri.cpp codec.cpp zip.cpp ansi.cpp tty.cpp require.cpp jsrcpack.cpp
    pol.cpp ${ABC_DIR}/POL.c net.cpp ${ABC_DIR}/NET.c ${ABC_DIR}/TCP.c ${ABC_DIR}/UDP.c
    fsw.cpp ${ABC_DIR}/FSW.c)

# JAB-001: the runtime binary is `jab` — a standalone runner divorced from `be`
# (internal JABC* C++ symbols + add_test names stay).
add_executable(jab ${JABC_SRC})
target_link_libraries(jab ${JSC_LIB} dog abc-core ${sodium})
set_target_properties(jab PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DOG_BIN_DIR})

# JAB-035: -DJAB_JSRC=<dir> embeds that tree as the DEFAULT jsrc pack — the
# floor of the jsrc stack, so a release binary runs verbs with no checkout.
# Two-stage, because the packer is jab itself: a bootstrap `jab_boot` (no pack)
# runs jsrcpack.js over <dir>, and jsrcpack.S .incbin's the result into `jab`.
# Without the option nothing here happens: no .S, no symbol, no floor.
if(JAB_JSRC)
    enable_language(ASM)
    get_filename_component(JAB_JSRC_DIR "${JAB_JSRC}" ABSOLUTE)
    if(NOT IS_DIRECTORY ${JAB_JSRC_DIR})
        message(FATAL_ERROR "JAB_JSRC is not a directory: ${JAB_JSRC_DIR}")
    endif()
    set(JAB_PACK ${CMAKE_CURRENT_BINARY_DIR}/jsrc.pack)
    add_executable(jab_boot ${JABC_SRC})
    target_link_libraries(jab_boot ${JSC_LIB} dog abc-core ${sodium})
    file(GLOB_RECURSE JAB_JSRC_FILES CONFIGURE_DEPENDS ${JAB_JSRC_DIR}/*)
    #  The packer runs as an EXPLICIT path and requires nothing: a build box
    #  has no jsrc/ to climb to.
    add_custom_command(OUTPUT ${JAB_PACK}
        COMMAND jab_boot ${CMAKE_CURRENT_SOURCE_DIR}/jsrcpack.js
                ${JAB_JSRC_DIR} ${JAB_PACK}
        DEPENDS jab_boot ${CMAKE_CURRENT_SOURCE_DIR}/jsrcpack.js ${JAB_JSRC_FILES}
        COMMENT "JAB-035: packing ${JAB_JSRC_DIR}")
    add_custom_target(jab_jsrc_pack DEPENDS ${JAB_PACK})
    target_sources(jab PRIVATE jsrcpack.S)
    add_dependencies(jab jab_jsrc_pack)
    set_source_files_properties(jsrcpack.S PROPERTIES OBJECT_DEPENDS ${JAB_PACK})
    target_compile_definitions(jab PRIVATE
        JABC_JSRC_PACK JABC_JSRC_PACK_FILE="${JAB_PACK}")
endif()

# Table-driven C++ harness over the buf/utf8/io boundary; returns non-zero on
# any failed row, so CTest fails on a real bug (CLAUDE.md §3).
add_executable(jabc_test test/jabc_test.cpp arg.cpp io.cpp utf8.cpp buf.cpp codec.cpp)
target_link_libraries(jabc_test ${JSC_LIB} dog abc)

add_test(NAME JABCtestCpp COMMAND jabc_test)

# End-to-end: the jabc binary runs an inline Buf+utf8 round-trip; any failed
# assertion throws, which JABCRun turns into a non-zero exit (CTest fails).
add_test(NAME JABCe2e COMMAND jab --eval
    "let b=io.buf(16);b.feedStr('hié');if(b.size!==4)throw 'size';if(utf8.Decode(b.data())!=='hié')throw 'roundtrip';if(b.take(1)[0]!==104)throw 'take';if(b.size!==3)throw 'consume'")

# Container bindings (abc.* HEAP/HASH over all lanes) — each .js throws on a
# failed assertion, which JABCRun turns into a non-zero exit (CTest fails).
add_test(NAME JABCheap COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/heap.js)
add_test(NAME JABChash COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/hash.js)
add_test(NAME JABCmmap COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/mmap.js)
add_test(NAME JABCmmapfd COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/mmapfd.js)
# JAB-033: a mapping pins a VMA (and, for "rw", a booked fd) that the byte-only
# collector cannot see; both counts must plateau over a map-and-drop loop.
add_test(NAME JABCmmapvma COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/mmapvma.js)
add_test(NAME JABCstat COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/stat.js)
add_test(NAME JABCreaddir COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/readdir.js)
add_test(NAME JABCchdir COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/chdir.js)
add_test(NAME JABCsha COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/sha.js)
add_test(NAME JABChit COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/hit.js)
add_test(NAME JABCindex COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/index.js)
add_test(NAME JABChunk COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/hunk.js)
add_test(NAME JABCtok COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/tok.js)
add_test(NAME JABCuri COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/uri.js)
add_test(NAME JABCcodec COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/codec.js)
add_test(NAME JABCrequire COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/require.js)
add_test(NAME JABCpack COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/pack.js)
add_test(NAME JABCpackidx COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/packidx.js)
#  JAB-020: the one-call stream ingest (git.pack) over a synthetic pack.
add_test(NAME JABCrepack COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/repack.js)
add_test(NAME JABCgit COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/git.js)
add_test(NAME JABCulog COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ulog.js)
# ULOG-002/001: native ULOGOpen/AppendAt/Close bindings (booked log + sidecar).
add_test(NAME JABCulogBook COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ulog_book.js)
add_test(NAME JABCweave COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/weave.js)
# JAB-008: binding-boundary BASS rewind — a 256KB fold x1500 in one process
# (pre-fix the un-rewound arena drained at ~iter 960: "out full?").
add_test(NAME JABCbass COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/bass.js)
# JAB-008: probe sized to finish in ~7s post-fix; 10s cap is a hard order.
set_tests_properties(JABCbass PROPERTIES TIMEOUT 10)
add_test(NAME JABCansi COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ansi.js)
add_test(NAME JABCconsole COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/console.js)
add_test(NAME JABCtty COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/tty.js)
add_test(NAME JABCpol COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/pol.js)
# JAB-031: fsw — arm a temp dir, create a file, drain the name (inotify).
add_test(NAME JABCfsw COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/fsw.js)
set_tests_properties(JABCfsw PROPERTIES TIMEOUT 20)
add_test(NAME JABCnet COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/net.js)
add_test(NAME JABCnetGrow COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/netgrow.js)
# JS-035: top-level test/js dir (not js-local), so ${CMAKE_SOURCE_DIR}.
# JAB-005: beagle-owned test file — absent standalone, so existence-gated.
if(EXISTS ${CMAKE_SOURCE_DIR}/test/js/get/zip.js)
    add_test(NAME JABCzip COMMAND jab ${CMAKE_SOURCE_DIR}/test/js/get/zip.js)
    set_tests_properties(JABCzip PROPERTIES
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
endif()
# DOG-027: JABCulogLib/JABCstore are GONE — they ran ../be/test/*.js, i.e. a
# sibling repo's JS reached by local path, so a jab API change went red here
# with the only fix living in be/.  be/ tests belong to be/'s own suite.

# Script argv reaches JS: run a generated script with a tail of args and grep the
# JSON it logs to stderr (`args` + Node-shaped `process.argv`) — JS-015.
add_test(NAME JABCargv
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-015.sh $<TARGET_FILE:jab>)

# JAB-001: `jab <bareword>` resolves via the upward `jsrc/`-scan and patches
# process.argv[1] (the `here` idiom), distinct from explicit-path direct eval.
add_test(NAME JABCrequireJsrc
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-055.sh $<TARGET_FILE:jab>)

# JAB-035: the pack FORMAT (preamble + a deterministic ustar) over the very
# packer cmake runs at build time — every build, packed or not.
add_test(NAME JABCjsrcPack COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/jsrcpack.js)

# JAB-035: the floor end-to-end, in a scratch HOME (no jsrc anywhere, and the
# real ~/.cache untouched).  A packed build must resolve a bareword out of
# <cache>/jsrcs/<hash>/ and let a user jsrc/ override it file by file; an
# unpacked build must still die with "cannot find jsrc/main.js".
if(JAB_JSRC)
    add_test(NAME JABCjsrcFloor
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-035.sh $<TARGET_FILE:jab>)
else()
    add_test(NAME JABCjsrcNoFloor
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-035.sh $<TARGET_FILE:jab>
                --nofloor)
endif()

# JS-054: error-exit paths must run the same teardown as success, so a failed
# script-file open frees the JS context.  Deliberately NOT in the suppression
# group below — the broad JSC suppression would mask the leak; the script runs
# unsuppressed and self-calibrates against the clean exit.
add_test(NAME JABCleakExit
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-054.sh $<TARGET_FILE:jab>)

# JS-064: a bareword `jab <projector>` (the resident be/ loop) must be LSan-clean
# on the normal exit.  Like JABCleakExit, NOT in the suppression group: the test
# runs without an external suppressions file so it exercises the suppression now
# embedded in the binary (main.cpp __lsan_default_suppressions).
add_test(NAME JABCleakLoop
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-064.sh $<TARGET_FILE:jab>)

# JavaScriptCore leaks lazily-initialised global singletons that LSan flags
# but are not our leaks; suppress JSC-internal allocations by library name.
set_tests_properties(JABCtestCpp JABCe2e JABCheap JABChash JABCmmap JABCstat JABCreaddir JABCsha JABChit JABCindex JABChunk JABCtok JABCuri JABCcodec JABCrequire JABCpack JABCpackidx JABCrepack JABCgit JABCulog JABCulogBook JABCweave JABCbass JABCansi JABCconsole JABCtty JABCpol JABCfsw JABCnet JABCargv JABCrequireJsrc JABCjsrcPack PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
