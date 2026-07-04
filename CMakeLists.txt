enable_language(CXX)

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

if(NOT JSC_LIB)
    message(WARNING "js: JavaScriptCore not found, skipping")
    return()
endif()

include_directories(
    ${JSC_INCLUDE_DIR}
    ..
    .
)

# The JS Buf class is embedded in buf.cpp as a raw string literal; no js2c /
# node codegen step (node is not a build dependency).
# abc/POL.c (pol) and NET/TCP/UDP.c (net/dgram) are stripped from abc-core
# (grouped with CURL); compile them straight into jab for the socket/loop leaves.
set(JABC_SRC main.cpp io.cpp utf8.cpp buf.cpp console.cpp cont.cpp tok.cpp uri.cpp codec.cpp zip.cpp ansi.cpp tty.cpp require.cpp
    pol.cpp ../abc/POL.c net.cpp ../abc/NET.c ../abc/TCP.c ../abc/UDP.c)

# JAB-001: the runtime binary is `jab` — a standalone runner divorced from `be`
# (internal JABC* C++ symbols + add_test names stay).
add_executable(jab ${JABC_SRC})
target_link_libraries(jab ${JSC_LIB} dog abc-core ${sodium})
set_target_properties(jab PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DOG_BIN_DIR})

# Table-driven C++ harness over the buf/utf8/io boundary; returns non-zero on
# any failed row, so CTest fails on a real bug (CLAUDE.md §3).
add_executable(jabc_test test/jabc_test.cpp io.cpp utf8.cpp buf.cpp codec.cpp)
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
add_test(NAME JABCstat COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/stat.js)
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
add_test(NAME JABCgit COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/git.js)
add_test(NAME JABCulog COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ulog.js)
# ULOG-002/001: native ULOGOpen/AppendAt/Close bindings (booked log + sidecar).
add_test(NAME JABCulogBook COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ulog_book.js)
add_test(NAME JABCweave COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/weave.js)
add_test(NAME JABCansi COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/ansi.js)
add_test(NAME JABCconsole COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/console.js)
add_test(NAME JABCtty COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/tty.js)
add_test(NAME JABCpol COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/pol.js)
add_test(NAME JABCnet COMMAND jab ${CMAKE_CURRENT_SOURCE_DIR}/test/net.js)
# JS-035: top-level test/js dir (not js-local), so ${CMAKE_SOURCE_DIR}.
add_test(NAME JABCzip COMMAND jab ${CMAKE_SOURCE_DIR}/test/js/get/zip.js)
# JS-048: bin/lib/ulog.js (ULOG read+write: drain readers + crash-safe
# monotonic appender) + bin/lib/store.js (object + ref read+write: ref
# resolve readers + createShard/set/tombstone writers), pure-JS unit tests
# on jabc.  (JABCulog is the abc ULOG-binding test under js/test/, distinct.)
# JS-063: be-shard unit tests moved to ../be/test/ (JS-061); guard each so a
# checkout without the be submodule skips rather than errors.
if(EXISTS ${CMAKE_SOURCE_DIR}/../be/test/ulog.js)
    add_test(NAME JABCulogLib COMMAND jab ${CMAKE_SOURCE_DIR}/../be/test/ulog.js)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/../be/test/store.js)
    add_test(NAME JABCstore COMMAND jab ${CMAKE_SOURCE_DIR}/../be/test/store.js)
endif()

# Script argv reaches JS: run a generated script with a tail of args and grep the
# JSON it logs to stderr (`args` + Node-shaped `process.argv`) — JS-015.
add_test(NAME JABCargv
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-015.sh $<TARGET_FILE:jab>)

# JAB-001: `jab <bareword>` resolves via the upward `be/`-scan and patches
# process.argv[1] (the `here` idiom), distinct from explicit-path direct eval.
add_test(NAME JABCrequireBe
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/run-055.sh $<TARGET_FILE:jab>)

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
# JS-095: JABCulogLib/JABCstore are be/-gated (created only with the be/ sibling);
# dropped from this unconditional list so configure does not abort without be/.
set_tests_properties(JABCtestCpp JABCe2e JABCheap JABChash JABCmmap JABCstat JABCsha JABChit JABCindex JABChunk JABCtok JABCuri JABCcodec JABCrequire JABCpack JABCpackidx JABCgit JABCulog JABCulogBook JABCweave JABCansi JABCconsole JABCtty JABCpol JABCnet JABCzip JABCargv JABCrequireBe PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp")
