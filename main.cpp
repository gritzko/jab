#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JABC.hpp"
extern "C" {
#include "abc/PRO.h"
}

thread_local JSGlobalContextRef JABC_CONTEXT;
thread_local JSObjectRef JABC_GLOBAL_OBJECT;

//  PRO.h globals (one definition for the whole binary).
u8 _pro_depth = 0;
extern "C" _Thread_local u8* ABC_BASS[4] = {};

static JSValueRef JSPropertyMessage = NULL;
static JSValueRef JSPropertyStack = NULL;

static void JSInit() {
  JABC_CONTEXT = JSGlobalContextCreate(NULL);
  JABC_GLOBAL_OBJECT = JSContextGetGlobalObject(JABC_CONTEXT);

  JSStringRef m = JSStringCreateWithUTF8CString("message");
  JSPropertyMessage = JSValueMakeString(JABC_CONTEXT, m);
  JSStringRelease(m);
  JSValueProtect(JABC_CONTEXT, JSPropertyMessage);

  JSStringRef s = JSStringCreateWithUTF8CString("stack");
  JSPropertyStack = JSValueMakeString(JABC_CONTEXT, s);
  JSStringRelease(s);
  JSValueProtect(JABC_CONTEXT, JSPropertyStack);
}

static void JSClose() {
  JSValueUnprotect(JABC_CONTEXT, JSPropertyStack);
  JSValueUnprotect(JABC_CONTEXT, JSPropertyMessage);
  JSGlobalContextRelease(JABC_CONTEXT);
  JABC_CONTEXT = NULL;
  JABC_GLOBAL_OBJECT = NULL;
  JSPropertyMessage = NULL;
  JSPropertyStack = NULL;
}

//  Report a JS exception (message + stack if present) to stderr.
void JABCReport(JSValueRef exception) {
  char page[PAGESIZE];
  if (JSValueIsObject(JABC_CONTEXT, exception) &&
      JSObjectHasPropertyForKey(JABC_CONTEXT, (JSObjectRef)exception,
                                JSPropertyStack, NULL)) {
    JSValueRef ref = JSObjectGetPropertyForKey(
        JABC_CONTEXT, (JSObjectRef)exception, JSPropertyStack, NULL);
    JSStringRef rs = JSValueToStringCopy(JABC_CONTEXT, ref, NULL);
    JSStringGetUTF8CString(rs, page, PAGESIZE);
    JSStringRelease(rs);
    fprintf(stderr, "JS exception: %s\n", page);
    return;
  }
  JSStringRef es = JSValueToStringCopy(JABC_CONTEXT, exception, NULL);
  JSStringGetUTF8CString(es, page, PAGESIZE);
  JSStringRelease(es);
  fprintf(stderr, "JS exception: %s\n", page);
}

void JABCExecute(const char* script) {
  JSStringRef code = JSStringCreateWithUTF8CString(script);
  JSValueRef exception = NULL;
  JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  if (exception != NULL) JABCReport(exception);
  JSStringRelease(code);
}

//  Like JABCExecute but reports whether an uncaught exception occurred, so a
//  failing `--eval`/script run propagates to the process exit code (CTest).
static b8 JABCRun(const char* script) {
  JSStringRef code = JSStringCreateWithUTF8CString(script);
  JSValueRef exception = NULL;
  JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  JSStringRelease(code);
  if (exception != NULL) {
    JABCReport(exception);
    return NO;
  }
  return YES;
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

//  Expose the script's argv tail to JS: global `args` (tokens after the script
//  path) plus a Node-ish `process = { argv: ["jab", script, ...tail] }`.
//  `tail` is the index of the first token after the script path (== argc when
//  there is none, e.g. under --eval), so `args` is empty in that case.
static void JABCInstallArgv(int argc, char** argv, int tail,
                            const char* script_file) {
  JABCSetGlobal("args", JABCArgvArray(argc, argv, tail, NULL, NULL));
  JSObjectRef proc = JSObjectMake(JABC_CONTEXT, NULL, NULL);
  JSStringRef k = JSStringCreateWithUTF8CString("argv");
  JSObjectSetProperty(JABC_CONTEXT, proc, k,
                      JABCArgvArray(argc, argv, tail, "jab", script_file),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
  JABCSetGlobal("process", proc);
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

  //  JAB-001: an EXPLICIT path (/abs, ./rel, ../up) runs the file directly via
  //  global eval; a BARE name runs the require machine (`__main` resolves it
  //  through the upward be/-scan and patches process.argv[1]).
  if (script_file != NULL) {
    b8 explicit_path = script_file[0] == '/' ||
        (script_file[0] == '.' && script_file[1] == '/') ||
        (script_file[0] == '.' && script_file[1] == '.' &&
         script_file[2] == '/');
    if (explicit_path) {
      if (!JABCRunFile(script_file)) rc = 1;
    } else {
      JABCSetGlobal("__mainSpec", JSOfCString(script_file));
      if (!JABCRun("__main(__mainSpec)")) rc = 1;
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
