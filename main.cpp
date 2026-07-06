#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JABC.hpp"
extern "C" {
#include "abc/PRO.h"
#include "dog/VERSN.h"   // process.version / .build / .build_date
}

//  JS-064: embed the JSC-singleton suppression so EVERY run (a bareword `jab`,
//  not just ctest) is leak-clean — JSC never frees its lazy VM singletons, and
//  the loop pins more than the ctest env's lsan.supp masks.  See js/lsan.supp.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
extern "C" const char* __lsan_default_suppressions(void) {
  return "leak:libjavascriptcoregtk\n";
}
#  endif
#endif

thread_local JSGlobalContextRef JABC_CONTEXT;
thread_local JSObjectRef JABC_GLOBAL_OBJECT;

//  PRO.h globals (one definition for the whole binary).
u8 _pro_depth = 0;
extern "C" thread_local u8* ABC_BASS[4] = {};

static void JSInit() {
  JABC_CONTEXT = JSGlobalContextCreate(NULL);
  JABC_GLOBAL_OBJECT = JSContextGetGlobalObject(JABC_CONTEXT);
}

static void JSClose() {
  JSGlobalContextRelease(JABC_CONTEXT);
  JABC_CONTEXT = NULL;
  JABC_GLOBAL_OBJECT = NULL;
}

//  Report an uncaught JS exception to stderr as `String(value)` — for an
//  Error that is `"<name>: <message>"`.  We deliberately do NOT read the
//  Error's `.stack` (nor `.line`/`.sourceURL`): touching any of them lazily
//  materialises JSC source-provider state the VM caches and never frees
//  before exit, so a failing `jab <script>` would trip LeakSanitizer on a
//  clean teardown (the JSC-internal-leak class js/lsan.supp targets — but
//  here we avoid creating it at all).  The name+message is the actionable
//  part; the C-API `.stack` text (`eval code@…`) was low-value anyway.
void JABCReport(JSValueRef exception) {
  char page[PAGESIZE];
  JSStringRef es = JSValueToStringCopy(JABC_CONTEXT, exception, NULL);
  if (es == NULL) {  //  value's toString itself threw — nothing printable
    fprintf(stderr, "JS exception: <unprintable>\n");
    return;
  }
  JSStringGetUTF8CString(es, page, PAGESIZE);
  JSStringRelease(es);
  fprintf(stderr, "JS exception: %s\n", page);
#ifndef NDEBUG
  //  Debug build only: also dump the Error's `.stack`.  Reading it pins JSC
  //  source state that leaks on teardown (js/lsan.supp covers it) — acceptable
  //  in a debug build for a real trace; release (NDEBUG) stays leak-free.
  if (JSValueIsObject(JABC_CONTEXT, exception)) {
    JSObjectRef obj = JSValueToObject(JABC_CONTEXT, exception, NULL);
    JSStringRef key = JSStringCreateWithUTF8CString("stack");
    JSValueRef st = obj ? JSObjectGetProperty(JABC_CONTEXT, obj, key, NULL) : NULL;
    JSStringRelease(key);
    if (st != NULL && !JSValueIsUndefined(JABC_CONTEXT, st)) {
      JSStringRef ss = JSValueToStringCopy(JABC_CONTEXT, st, NULL);
      if (ss != NULL) {
        char sp[PAGESIZE];
        JSStringGetUTF8CString(ss, sp, PAGESIZE);
        JSStringRelease(ss);
        fprintf(stderr, "%s\n", sp);
      }
    }
  }
#endif
}

void JABCExecute(const char* script) {
  JSStringRef code = JSStringCreateWithUTF8CString(script);
  JSValueRef exception = NULL;
  JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  if (exception != NULL) JABCReport(exception);
  JSStringRelease(code);
}

//  Run user code (a `--eval` expression, a script body, or `__main(...)`) and
//  report whether it succeeded, so a failing run sets the process exit code
//  (CTest).  Returns YES on success, NO after reporting a thrown error.
//
//  Evaluating inside a JS try/catch is load-bearing for a leak-free teardown,
//  not cosmetic.  Two independent JSC artifacts otherwise leak on a failing
//  run (LeakSanitizer flags them on a bare `jab <bad>`; the test harness hides
//  them via js/lsan.supp): (1) an exception that escapes JSEvaluateScript to
//  the C boundary is retained by JSC until exit, so it must not reach C; and
//  (2) reading an Error's `.stack` (or `.line`/`.sourceURL`) pins JSC
//  source-provider state the VM never frees, so we report `String(e)` (name +
//  message), never the stack.  Indirect eval `(0,eval)(src)` runs the code in
//  GLOBAL scope — same program semantics as a bare JSEvaluateScript — and the
//  catch hands back the message text (or null on success).  The source rides
//  the `__src` global so arbitrary code needs no escaping.
static b8 JABCRun(const char* script) {
  {
    JSStringRef k = JSStringCreateWithUTF8CString("__src");
    JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, k,
                        JSOfCString(script), kJSPropertyAttributeNone, NULL);
    JSStringRelease(k);
  }
#ifndef NDEBUG
  //  Debug build: append the Error's `.stack` to the reported text (leaks JSC
  //  source state on teardown — js/lsan.supp covers it — worth it for a trace).
  static const char WRAP[] =
      "(function(){try{(0,eval)(__src);return null;}"
      "catch(e){return String(e)+(e&&e.stack?'\\n'+e.stack:'');}})()";
#else
  static const char WRAP[] =
      "(function(){try{(0,eval)(__src);return null;}"
      "catch(e){return String(e);}})()";
#endif
  JSStringRef code = JSStringCreateWithUTF8CString(WRAP);
  JSValueRef exception = NULL;
  JSValueRef r =
      JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  JSStringRelease(code);
  //  The fixed wrapper itself never throws; any escape is a hard failure.
  if (exception != NULL) {
    JABCReport(exception);
    return NO;
  }
  if (JSValueIsNull(JABC_CONTEXT, r)) return YES;
  //  `r` is the caught error's `String(e)` text (no `.stack` touched).
  char page[PAGESIZE];
  JSStringRef es = JSValueToStringCopy(JABC_CONTEXT, r, NULL);
  if (es != NULL) {
    JSStringGetUTF8CString(es, page, PAGESIZE);
    JSStringRelease(es);
    fprintf(stderr, "JS exception: %s\n", page);
  }
  return NO;
}

//  Read a script file and run it; NO on any open/size/OOM/exception failure.
//  The read lives in a worker (not inline in main) so every failure returns
//  to main's shared teardown instead of a bare `return` that would leak the
//  whole JS context (JS-054; cf. the wrapper/worker idiom, CLAUDE.md §5).
static b8 JABCRunFile(const char* script_file) {
  FILE* f = fopen(script_file, "rb");
  if (!f) {
    fprintf(stderr, "Error: cannot open %s\n", script_file);
    return NO;
  }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  if (len < 0) {
    fprintf(stderr, "Error: cannot size %s\n", script_file);
    fclose(f);
    return NO;
  }
  rewind(f);
  char* script = (char*)malloc((size_t)len + 1);
  if (!script) {
    fprintf(stderr, "Error: out of memory reading %s\n", script_file);
    fclose(f);
    return NO;
  }
  size_t got = fread(script, 1, (size_t)len, f);
  fclose(f);
  script[got] = '\0';
  b8 ok = JABCRun(script);
  free(script);
  return ok;
}

static void JABCInstallModules() {
  JABCutf8Install();
  JABCioInstall();
  JABCbufInstall();   //  Buf class + constructors layer over utf8/io leaves
  JABCConsoleInstall();  //  console.* over utf8.Encode + io.writeAll (JAB-002)
  JABCContInstall();  //  abc.* containers (HEAP/HASH) layer over io mmap leaves
  JABCTokInstall();   //  tok.parse + TokStream over dog/tok (JS-023)
  JABCUriInstall();   //  URI class over abc/URI
  JABCCodecInstall(); //  hex + sha1/sha256 + ron
  JABCZipInstall();   //  zip.deflate/inflate raw zlib over dog/git/ZINF (JS-035)
  JABCAnsiInstall();  //  ansi colour helper (pure JS)
  JABCTtyInstall();   //  tty.raw/cook/size terminal control over abc/FILE (JS-053)
  JABCPolInstall();   //  poll() event loop over abc/POL (handlers do io.* I/O)
  JABCNetInstall();   //  net/dgram + Node timers over pol (sockets do io.* I/O)
  JABCRequireInstall(); //  sync CommonJS require() over io.mmap/utf8 (last)
}

#define VERSION_BOILERPLATE "jab v0.1.0\n"

//  Set `name` on the global object to `val`.  No held refs: the JSStringRef key
//  is released here, the value is owned by JS GC (cf. uri.cpp's JABCSetStr).
static void JABCSetGlobal(const char* name, JSValueRef val) {
  JSStringRef k = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, k, val,
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
}

//  Build a JS string array from argv[from, argc) (each token a fresh JS-owned
//  string) and return it.  `head` (e.g. "jab", script path) is prepended when
//  non-NULL — used to shape Node's process.argv = ["jab", script, ...tail].
static JSObjectRef JABCArgvArray(int argc, char** argv, int from,
                                 const char* head0, const char* head1) {
  JSObjectRef arr = JSObjectMakeArray(JABC_CONTEXT, 0, NULL, NULL);
  unsigned at = 0;
  if (head0 != NULL)
    JSObjectSetPropertyAtIndex(JABC_CONTEXT, arr, at++, JSOfCString(head0), NULL);
  if (head1 != NULL)
    JSObjectSetPropertyAtIndex(JABC_CONTEXT, arr, at++, JSOfCString(head1), NULL);
  for (int i = from; i < argc; i++)
    JSObjectSetPropertyAtIndex(JABC_CONTEXT, arr, at++, JSOfCString(argv[i]),
                               NULL);
  return arr;
}

//  Set a read-only build-metadata string `key` on `proc` from a dog/VERSN
//  slice.  The slices back NUL-terminated literals, so `[0]` is a valid C
//  string; no held refs (key released here, value owned by JS GC).
static void JABCProcVersn(JSObjectRef proc, const char* key,
                          u8 const* const* v) {
  JSStringRef k = JSStringCreateWithUTF8CString(key);
  JSObjectSetProperty(JABC_CONTEXT, proc, k, JSOfCString((const char*)v[0]),
                      kJSPropertyAttributeReadOnly |
                          kJSPropertyAttributeDontDelete,
                      NULL);
  JSStringRelease(k);
}

//  Expose the script's argv tail to JS: global `args` (tokens after the script
//  path) plus a Node-ish `process = { argv: ["jab", script, ...tail] }`.
//  `tail` is the index of the first token after the script path (== argc when
//  there is none, e.g. under --eval), so `args` is empty in that case.  The
//  same `process` carries the build stamp: `version` / `build` / `build_date`.
static void JABCInstallArgv(int argc, char** argv, int tail,
                            const char* script_file) {
  JABCSetGlobal("args", JABCArgvArray(argc, argv, tail, NULL, NULL));
  JSObjectRef proc = JSObjectMake(JABC_CONTEXT, NULL, NULL);
  JSStringRef k = JSStringCreateWithUTF8CString("argv");
  JSObjectSetProperty(JABC_CONTEXT, proc, k,
                      JABCArgvArray(argc, argv, tail, "jab", script_file),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  JABCProcVersn(proc, "version", VERSNVersion);
  JABCProcVersn(proc, "build", VERSNHash);
  JABCProcVersn(proc, "build_date", VERSNDate);
  JABCSetGlobal("process", proc);
}

//  YES iff `s` ends in the literal ".js" — the script-vs-main entry switch: a
//  `.js` first arg is a SCRIPT, anything else routes to be/main.js (the loop).
static b8 JABCEndsWithJs(const char* s) {
  size_t n = strlen(s);
  return n >= 3 && strcmp(s + n - 3, ".js") == 0;
}

//  YES iff `s` opens with a URI scheme (`<alpha><alnum|+|-|.>*:`) BEFORE any
//  '/': a `scheme:` token (e.g. `diff:view/bro.js`) is a VIEW URI for the loop,
//  never a script file — even when its path tail ends in `.js`.  A bare/relative
//  path (`foo.js`, `./x.js`, `dir/x.js`) has no scheme.  Guards the `.js`-suffix
//  script route below so `jab diff:<file>.js` reaches the loop, not require().
static b8 JABCHasScheme(const char* s) {
  if (s == NULL || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
    return 0;
  for (const char* p = s + 1; *p; p++) {
    char c = *p;
    if (c == ':') return 1;
    if (c == '/') return 0;            // a path slash before any ':' — not a scheme
    b8 ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    if (!ok) return 0;
  }
  return 0;
}

int main(int argc, char** argv) {
  if (u8bMap(ABC_BASS, ABC_BASS_BYTES) != OK) {
    fprintf(stderr, "ABC_BASS u8bMap failed\n");
    return 1;
  }

  char* eval_code = NULL;
  char* script_file = NULL;
  //  Index of the first token after the script path (the argv "tail" exposed to
  //  JS as `args`).  Stays at argc when there is no script file (e.g. --eval).
  int tail = argc;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      fprintf(stderr, VERSION_BOILERPLATE);
      return 0;
    } else if (strcmp(argv[i], "--eval") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "Error: --eval requires a code argument\n");
        return 1;
      }
      eval_code = argv[++i];
    } else {
      script_file = argv[i];
      tail = i + 1;
      break;
    }
  }

  JSInit();
  JABCInstallModules();
  //  Argv tail reaches JS as `args` + Node-ish `process.argv` (JS-015).
  JABCInstallArgv(argc, argv, tail, script_file);

  int rc = 0;
  if (eval_code != NULL && !JABCRun(eval_code)) rc = 1;

  //  JAB: the first positional decides the entry shape.  A `.js` first arg is a
  //  SCRIPT: an EXPLICIT path (/abs, ./rel, ../up) runs the file directly via
  //  global eval (require rebased to its own dir); a bare/relative `.js` (e.g.
  //  `foo.js`, `be/main.js`) resolves via the upward be/-scan (`__runScript`).
  //  ANYTHING else — a verb, a `scheme:` URI, a non-.js path, or no arg at all
  //  — routes to be/main.js (`__main`) with the user's tokens passed through
  //  as-is at argv[2:]; the resident loop triages the verb/URI/path/no-arg.
  if (rc == 0) {
    if (script_file != NULL && JABCEndsWithJs(script_file) &&
        !JABCHasScheme(script_file)) {
      b8 explicit_path = script_file[0] == '/' ||
          (script_file[0] == '.' && script_file[1] == '/') ||
          (script_file[0] == '.' && script_file[1] == '.' &&
           script_file[2] == '/');
      JABCSetGlobal("__mainSpec", JSOfCString(script_file));
      if (explicit_path) {
        //  Bind the top-level require to the script's own dir so a sibling
        //  `require("./lib/x.js")` resolves script-relative under global eval.
        if (!JABCRun("__rebaseRequire(__mainSpec)") || !JABCRunFile(script_file))
          rc = 1;
      } else if (!JABCRun("__runScript(__mainSpec)")) {
        rc = 1;
      }
    } else if (eval_code == NULL) {
      //  verb / scheme:URI / non-.js path / bare `jab` → be/main.js (the loop).
      if (!JABCRun("__main()")) rc = 1;
    }
  }

  //  Node-like: once the top-level script returns, drive the event loop until
  //  no fds/timers remain.  pol.run on an already-drained queue is an instant
  //  no-op, so scripts that never touch net/timers are unaffected.
  if (rc == 0 && !JABCRun("pol.run(pol.NEVER)")) rc = 1;

  //  Drop the protected pol router refs + free the poll heap while the context
  //  is still alive (they are JS values).
  JABCPolUninstall();
  //  Release the context first so GC runs the no-copy deallocators
  //  (FILEUnMap/munmap) while the FILE subsystem is still alive; then tear it
  //  down (FILECloseAll frees FILE_RW).
  JSClose();
  JABCioUninstall();
  u8bUnMap(ABC_BASS);
  return rc;
}
