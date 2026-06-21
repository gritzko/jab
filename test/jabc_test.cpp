//  Table-driven harness over the buf / utf8 / io boundary.  Each row is a JS
//  expression that must evaluate to `true`; a thrown exception or a non-true
//  result fails the row.  Native-fd rows (pipe, mmap) inject fds/paths as JS
//  globals.  Exits non-zero on any failure so CTest sees a real bug.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "JABC.hpp"
extern "C" {
#include "abc/PRO.h"
}

thread_local JSGlobalContextRef JABC_CONTEXT;
thread_local JSObjectRef JABC_GLOBAL_OBJECT;
u8 _pro_depth = 0;
extern "C" _Thread_local u8* ABC_BASS[4] = {};

static int failures = 0;

void JABCReport(JSValueRef exception) {
  char page[PAGESIZE];
  JSStringRef es = JSValueToStringCopy(JABC_CONTEXT, exception, NULL);
  JSStringGetUTF8CString(es, page, PAGESIZE);
  JSStringRelease(es);
  fprintf(stderr, "    exception: %s\n", page);
}

void JABCExecute(const char* script) {
  JSStringRef code = JSStringCreateWithUTF8CString(script);
  JSValueRef exception = NULL;
  JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  if (exception != NULL) JABCReport(exception);
  JSStringRelease(code);
}

static void setGlobalNum(const char* name, double v) {
  JSStringRef k = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, k,
                      JSValueMakeNumber(JABC_CONTEXT, v),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
}

static void setGlobalStr(const char* name, const char* v) {
  JSStringRef k = JSStringCreateWithUTF8CString(name);
  JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, k, JSOfCString(v),
                      kJSPropertyAttributeNone, NULL);
  JSStringRelease(k);
}

//  Evaluate `js`; pass iff no exception and the result is boolean true.
static void check(const char* name, const char* js) {
  JSStringRef code = JSStringCreateWithUTF8CString(js);
  JSValueRef exception = NULL;
  JSValueRef r = JSEvaluateScript(JABC_CONTEXT, code, NULL, NULL, 1, &exception);
  JSStringRelease(code);
  if (exception != NULL) {
    fprintf(stderr, "FAIL %s\n", name);
    JABCReport(exception);
    failures++;
    return;
  }
  if (!JSValueToBoolean(JABC_CONTEXT, r)) {
    fprintf(stderr, "FAIL %s (not true)\n", name);
    failures++;
    return;
  }
  fprintf(stderr, "ok   %s\n", name);
}

struct Row {
  const char* name;
  const char* js;
};

//  In-memory rows: utf8 + Buf, no fd.
static const Row ROWS[] = {
    {"encode_len", "utf8.Encode('hello').length===5"},
    {"encode_multibyte", "utf8.Encode('é').length===2"},
    {"encode_astral", "utf8.Encode('\U0001F600').length===4"},
    {"decode_roundtrip",
     "utf8.Decode(utf8.Encode('héllo ☃'))==='héllo ☃'"},
    {"decode_overlong",
     "(()=>{try{utf8.Decode(new Uint8Array([0xC0,0x80]));return false}"
     "catch(e){return true}})()"},
    {"decode_lone_surrogate",
     "(()=>{try{utf8.Decode(new Uint8Array([0xED,0xA0,0x80]));return false}"
     "catch(e){return true}})()"},
    {"encodeInto_partial_ascii",
     "(()=>{let b=io.buf(2);return b.feedStr('abc')===2&&b.size===2})()"},
    {"encodeInto_no_half_multibyte",
     "(()=>{let b=io.buf(1);return b.feedStr('é')===0&&b.size===0})()"},
    {"buf_feed1_feedStr",
     "(()=>{let b=io.buf(8);b.feed1(65);b.feedStr('BC');"
     "return b.size===3&&utf8.Decode(b.data())==='ABC'})()"},
    {"buf_feed_buf",
     "(()=>{let a=io.buf(8);a.feedStr('xy');let b=io.buf(8);b.feed(a);"
     "return utf8.Decode(b.data())==='xy'})()"},
    {"buf_take",
     "(()=>{let b=io.buf(8);b.feedStr('abcd');let v=b.take(2);"
     "return v.length===2&&v[0]===97&&b.size===2&&utf8.Decode(b.data())==='cd'})()"},
    {"buf_feed_noroom",
     "(()=>{let b=io.buf(2);try{b.feed(utf8.Encode('abc'));return false}"
     "catch(e){return true}})()"},
    {"buf_shift",
     "(()=>{let b=io.buf(8);b.feedStr('abcd');b.skip(2);b.shift();"
     "return b._data===0&&b.size===2&&utf8.Decode(b.data())==='cd'})()"},
    {"buf_pop",
     "(()=>{let b=io.buf(8);b.feedStr('abc');return b.pop()===99&&b.size===2})()"},
    {"buf_shed",
     "(()=>{let b=io.buf(8);b.feedStr('abcd');b.shed(2);"
     "return b.size===2&&utf8.Decode(b.data())==='ab'})()"},
    {"buf_splice",
     "(()=>{let b=io.buf(16);b.feedStr('hello');b.splice(1,3,utf8.Encode('XY'));"
     "return utf8.Decode(b.data())==='hXYo'})()"},
    {"buf_grow",
     "(()=>{let b=io.buf(2);b.feedStr('ab');b.grow(8);b.feedStr('cd');"
     "return b.cap>=8&&utf8.Decode(b.data())==='abcd'})()"},
    {"ram_roundtrip",
     "(()=>{let b=io.ram(1<<20);b.feedStr('ram');"
     "return b.size===3&&utf8.Decode(b.data())==='ram'})()"},
};

int main() {
  if (u8bMap(ABC_BASS, ABC_BASS_BYTES) != OK) {
    fprintf(stderr, "ABC_BASS u8bMap failed\n");
    return 1;
  }
  //  JS-017 env fixture: set a known var BEFORE building the context so
  //  io.getenv() (getenv(3) under the hood) sees it.
  setenv("JABC017_VAR", "hello-017", 1);
  unsetenv("JABC017_UNSET");
  JABC_CONTEXT = JSGlobalContextCreate(NULL);
  JABC_GLOBAL_OBJECT = JSContextGetGlobalObject(JABC_CONTEXT);
  JABCutf8Install();
  JABCioInstall();
  JABCbufInstall();
  JABCCodecInstall();  //  JS-021: ron.now/of/date over the time leaves

  for (size_t i = 0; i < sizeof(ROWS) / sizeof(ROWS[0]); i++)
    check(ROWS[i].name, ROWS[i].js);

  //  JS-021 ron time codec: now() is a BigInt; of(Date)===of(ms-int);
  //  date(now()) is non-empty; date(0n) is the fixed "?" placeholder
  //  (ts<=0, centre-padded to 7 cols by DOGutf8sFeedDate).
  check("ron_now_bigint", "typeof ron.now()==='bigint'");
  //  1700000000000 ms = 2023-11-14, inside ron60's 2000-2099 YY range.
  check("ron_of_date_eq_ms",
        "ron.of(new Date(1700000000000))===ron.of(1700000000000)");
  check("ron_date_nonempty",
        "(()=>{let s=ron.date(ron.now());return typeof s==='string'&&s.trim().length>0})()");
  check("ron_date_zero_placeholder", "ron.date(0n)==='   ?   '");
  //  of(now-ms) then date() lands in the HH:MM bucket (same minute, <12h):
  //  trimmed to 5 chars "HH:MM" with a ':' at index 2.
  check("ron_date_now_hhmm",
        "(()=>{let s=ron.date(ron.of(Date.now())).trim();"
        "return s.length===5&&s[2]===':'})()");

  //  fd round-trip over a pipe: write a Buf's DATA, read it back into a Buf.
  int fds[2];
  if (pipe(fds) == 0) {
    setGlobalNum("WFD", fds[1]);
    setGlobalNum("RFD", fds[0]);
    check("pipe_write_read",
          "(()=>{let w=io.buf(16);w.feedStr('ping');io.writeAll(WFD,w);"
          "let r=io.buf(16);io.read(RFD,r);"
          "return r.size===4&&utf8.Decode(r.data())==='ping'})()");
    close(fds[0]);
    close(fds[1]);
  }

  //  mmap round-trip: write a file via fd, map it RO, read the bytes back.
  char path[] = "/tmp/jabc_mmap_XXXXXX";
  int tmp = mkstemp(path);
  if (tmp >= 0) {
    close(tmp);
    setGlobalStr("MMAP_PATH", path);
    check("mmap_roundtrip",
          "(()=>{let fd=io.open(MMAP_PATH,'c');let w=io.buf(16);"
          "w.feedStr('mapped!');io.writeAll(fd,w);io.close(fd);"
          "let m=io.mmap(MMAP_PATH,'r');"
          "return m.size===7&&utf8.Decode(m.data())==='mapped!'})()");
    unlink(path);
  }
  //  readdir over a fixture dir: files alpha/beta + a subdir sub/ with a child,
  //  plus a hidden file .hidden and a hidden subdir .hsub/ with its own child.
  //  Exercises the polymorphic 2nd arg: 1-level dir-marked array, a bare-fn
  //  callback scan, the {callback} object equivalent, the {recursive} flat
  //  listing, recursive+callback, and the {hidden} dotfile filter.
  char dir[] = "/tmp/jabc_dir_XXXXXX";
  if (mkdtemp(dir) != NULL) {
    char f1[64], f2[64], sub[64], kid[80], dot[80], hsub[80], hkid[96];
    snprintf(f1, sizeof(f1), "%s/alpha", dir);
    snprintf(f2, sizeof(f2), "%s/beta", dir);
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(kid, sizeof(kid), "%s/sub/child", dir);
    snprintf(dot, sizeof(dot), "%s/.hidden", dir);
    snprintf(hsub, sizeof(hsub), "%s/.hsub", dir);
    snprintf(hkid, sizeof(hkid), "%s/.hsub/secret", dir);
    close(open(f1, O_CREAT | O_WRONLY, 0600));
    close(open(f2, O_CREAT | O_WRONLY, 0600));
    mkdir(sub, 0700);
    close(open(kid, O_CREAT | O_WRONLY, 0600));
    close(open(dot, O_CREAT | O_WRONLY, 0600));
    mkdir(hsub, 0700);
    close(open(hkid, O_CREAT | O_WRONLY, 0600));
    setGlobalStr("DIR_PATH", dir);
    //  (a) one level: hidden default skips .hidden / .hsub/ ; `sub/` marked.
    check("readdir_names",
          "(()=>{let xs=io.readdir(DIR_PATH).slice().sort();"
          "return xs.length===3&&xs[0]==='alpha'&&xs[1]==='beta'&&xs[2]==='sub/'})()");
    check("readdir_dirs_marked",
          "(()=>{let xs=io.readdir(DIR_PATH);"
          "return xs.includes('sub/')&&xs.includes('alpha')&&!xs.includes('sub')"
          "&&xs.filter(n=>n.endsWith('/')).length===1})()");
    check("readdir_missing_throws",
          "(()=>{try{io.readdir(DIR_PATH+'/nope');return false}catch(e){return true}})()");
    //  hidden default skips dotfiles; {hidden:true} includes them.
    check("readdir_hidden_default_skips",
          "(()=>{let xs=io.readdir(DIR_PATH);"
          "return !xs.includes('.hidden')&&!xs.includes('.hsub/')})()");
    check("readdir_hidden_true_lists",
          "(()=>{let xs=io.readdir(DIR_PATH,{hidden:true});"
          "return xs.includes('.hidden')&&xs.includes('.hsub/')})()");
    //  (b) callback form: `enough` stops early (sees < all siblings).
    check("readdir_cb_enough_stops",
          "(()=>{let seen=0;io.readdir(DIR_PATH,n=>{seen++;return 'enough'});"
          "return seen===1})()");
    check("readdir_cb_more_sees_all",
          "(()=>{let seen=0;io.readdir(DIR_PATH,n=>{seen++;return 'more'});"
          "return seen===3})()");
    check("readdir_cb_throw_propagates",
          "(()=>{try{io.readdir(DIR_PATH,n=>{throw new Error('boom')});return false}"
          "catch(e){return e.message==='boom'}})()");
    //  {callback:fn} behaves EXACTLY like the bare-fn form (same entries).
    check("readdir_callback_obj_eq_bare",
          "(()=>{let a=[];io.readdir(DIR_PATH,n=>{a.push(n);return 'more'});"
          "let b=[];io.readdir(DIR_PATH,{callback:n=>{b.push(n);return 'more'}});"
          "return a.slice().sort().join(',')===b.slice().sort().join(',')"
          "&&a.length===3})()");
    check("readdir_callback_obj_returns_undefined",
          "(()=>{return io.readdir(DIR_PATH,{callback:n=>'more'})===undefined})()");
    //  {callback} hidden default still skips the dotfiles.
    check("readdir_callback_obj_hidden_default",
          "(()=>{let a=[];io.readdir(DIR_PATH,{callback:n=>{a.push(n)}});"
          "return !a.includes('.hidden')&&!a.includes('.hsub/')})()");
    //  (c) recursion reaches sub/child — via the cb `recur` directive, the
    //  {recursive:true} flat listing, AND {recursive:true, callback}.
    check("readdir_cb_recur_reaches_child",
          "(()=>{let names=[];io.readdir(DIR_PATH,n=>{names.push(n);"
          "return n.endsWith('/')?'recur':'more'});"
          "return names.includes('sub/child')})()");
    check("readdir_recursive_flat",
          "(()=>{let xs=io.readdir(DIR_PATH,{recursive:true}).slice().sort();"
          "return xs.includes('sub/')&&xs.includes('sub/child')"
          "&&xs.includes('alpha')&&xs.includes('beta')})()");
    check("readdir_recursive_returns_array",
          "(()=>{return Array.isArray(io.readdir(DIR_PATH,{recursive:true}))})()");
    //  recursive + hidden default: never reaches the hidden subtree.
    check("readdir_recursive_hidden_pruned",
          "(()=>{let xs=io.readdir(DIR_PATH,{recursive:true});"
          "return !xs.includes('.hsub/')&&!xs.includes('.hsub/secret')"
          "&&!xs.includes('.hidden')})()");
    check("readdir_recursive_hidden_true_descends",
          "(()=>{let xs=io.readdir(DIR_PATH,{recursive:true,hidden:true});"
          "return xs.includes('.hsub/')&&xs.includes('.hsub/secret')"
          "&&xs.includes('.hidden')})()");
    //  recursive + callback: cb fires across the subtree; `enough` aborts all.
    check("readdir_recursive_callback_reaches_child",
          "(()=>{let names=[];let r=io.readdir(DIR_PATH,"
          "{recursive:true,callback:n=>{names.push(n)}});"
          "return r===undefined&&names.includes('sub/child')})()");
    check("readdir_recursive_callback_enough_aborts",
          "(()=>{let seen=0;io.readdir(DIR_PATH,"
          "{recursive:true,callback:n=>{seen++;return 'enough'}});"
          "return seen===1})()");
    //  a non-function, non-object 2nd arg is a TypeError-style throw.
    check("readdir_bad_2nd_arg_throws",
          "(()=>{try{io.readdir(DIR_PATH,42);return false}catch(e){return true}})()");
    unlink(hkid);
    unlink(dot);
    unlink(kid);
    unlink(f1);
    unlink(f2);
    rmdir(hsub);
    rmdir(sub);
    rmdir(dir);
  }

  //  JS-020: process spawn + reap.  spawn pipes the child's stdout back as a
  //  fd; reap surfaces a clean exit as {code} and a signal death as {signal}.
  //  Bin paths are resolved via execvp's PATH lookup.
  check("spawn_echo_stdout",
        "(()=>{let p=io.spawn('/bin/echo',['echo','hi']);"
        "let b=io.buf(64);io.read(p.stdout,b);io.close(p.stdout);"
        "if(p.stdin!==undefined)io.close(p.stdin);"  //  no stdin pipe requested
        "let r=io.reap(p.pid);"
        "return r.code===0&&utf8.Decode(b.data())==='hi\\n'})()");
  check("spawn_stdin_roundtrip",
        "(()=>{let p=io.spawn('/bin/cat',['cat']);"
        "let w=io.buf(32);w.feedStr('echo back\\n');io.writeAll(p.stdin,w);"
        "io.close(p.stdin);"  //  EOF for cat
        "let b=io.buf(64);io.read(p.stdout,b);io.close(p.stdout);"
        "let r=io.reap(p.pid);"
        "return r.code===0&&utf8.Decode(b.data())==='echo back\\n'})()");
  check("reap_nonzero_code",
        "(()=>{let p=io.spawn('/bin/sh',['sh','-c','exit 3']);"
        "io.close(p.stdout);io.close(p.stdin);"
        "let r=io.reap(p.pid);"
        "return r.code===3&&r.signal===undefined})()");
  check("reap_signal_death",
        "(()=>{let p=io.spawn('/bin/sh',['sh','-c','kill -9 $$']);"
        "io.close(p.stdout);io.close(p.stdin);"
        "let r=io.reap(p.pid);"
        "return r.signal===9&&r.code===undefined})()");
  check("spawnfds_inherit",
        "(()=>{let pid=io.spawnFds('/bin/sh',['sh','-c','exit 7'],-1,-1);"
        "let r=io.reap(pid);return r.code===7})()");
  check("spawn_argv_not_array_throws",
        "(()=>{try{io.spawn('/bin/echo','notarray');return false}catch(e){return true}})()");
  check("spawn_argv_empty_throws",
        "(()=>{try{io.spawn('/bin/echo',[]);return false}catch(e){return true}})()");

  //  JS-017: io.getenv(name) -> string|undefined ; io.cwd() -> string.
  //  A set var reads back; an unset var is undefined; cwd matches getcwd().
  check("getenv_set", "io.getenv('JABC017_VAR')==='hello-017'");
  check("getenv_unset", "io.getenv('JABC017_UNSET')===undefined");
  {
    char cwd[PAGESIZE];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      setGlobalStr("CWD", cwd);
      check("cwd_matches", "io.cwd()===CWD");
    }
  }
  //  Release the context FIRST: GC runs the no-copy deallocators
  //  (FILEUnMap/munmap), which touch the FILE subsystem — so tear that down
  //  (FILECloseAll frees FILE_RW) only afterwards.
  JSGlobalContextRelease(JABC_CONTEXT);
  JABCioUninstall();
  u8bUnMap(ABC_BASS);

  fprintf(stderr, "\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
          failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
