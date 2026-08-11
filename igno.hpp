#ifndef JABC_IGNO_HPP
#define JABC_IGNO_HPP
//  STATUS-020: per-FILE `.gitignore` leaves over dog/git/IGNO's igno_set —
//  open ONE named file / match / close.  The chain (repo borders, the
//  shallow->deep fold, the `.git`/`.be` meta test) is the JS matcher's; C
//  only mmaps the one file it is named and decides against that one set.
//
//    dog._igno_open(gitignorePath) -> Number handle (0 = absent/unreadable)
//    dog._igno_match(h, path, isDir) -> -1 no match / 0 negated / 1 ignore
//    dog._igno_close(h) -> undefined (idempotent)
//
//  hunk.hpp must precede this header (shared JABCArgU8).
#include "hunk.hpp"

extern "C" {
#include "dog/git/IGNO.h"
}

//  STATUS-020: the handle box.  `magic` makes a stale/double close a no-op,
//  so closed boxes go on a free list instead of back to malloc (a live UAF).
#define JABC_IGNO_MAGIC 0x49474e4f5345547aULL
typedef struct jabc_igno {
  u64 magic;
  struct jabc_igno* next;
  igno_set set;
} jabc_igno;

static jabc_igno* JABC_IGNO_SPARE = NULL;

static jabc_igno* JABCignoHandle(JSContextRef ctx, JSValueRef v,
                                 JSValueRef* ex) {
  double d = JSValueToNumber(ctx, v, ex);
  if (*ex || d <= 0) return NULL;
  jabc_igno* h = (jabc_igno*)(size_t)d;
  return h->magic == JABC_IGNO_MAGIC ? h : NULL;
}

//  _igno_open(gitignorePath) -> Number handle; 0 when the file is absent or
//  unreadable (the common case — most dirs carry no `.gitignore`).
static JABC_FN(JABCignoOpen) {
  if (argc < 1) JABC_THROW("igno: _igno_open needs a .gitignore path");
  u8 pb[FILE_PATH_MAX_LEN];
  u8s ps = {};
  if (!JABCArgU8(ps, ctx, args[0], pb, sizeof(pb), exception)) JABC_UNDEF;
  jabc_igno* h = JABC_IGNO_SPARE;
  if (h) JABC_IGNO_SPARE = h->next;
  else h = (jabc_igno*)calloc(1, sizeof(jabc_igno));
  if (!h) JABC_THROW("igno: out of memory reading a .gitignore file");
  h->magic = JABC_IGNO_MAGIC;
  h->next = NULL;
  u8cs path = {ps[0], ps[1]};
  if (IGNOSetOpen(&h->set, path) != OK) {   //  no file here — not an error
    h->magic = 0;
    h->next = JABC_IGNO_SPARE;
    JABC_IGNO_SPARE = h;
    return JSValueMakeNumber(ctx, 0);
  }
  return JSValueMakeNumber(ctx, (double)(size_t)h);
}

//  _igno_match(handle, path, isDir) -> tristate Number.  `path` is already
//  prefixed to the set's own directory by the JS caller.
static JABC_FN(JABCignoMatch) {
  if (argc < 2) JABC_THROW("igno: _igno_match needs a handle and a path");
  jabc_igno* h = JABCignoHandle(ctx, args[0], exception);
  if (!h) JABC_THROW("igno: _igno_match got a closed or bogus handle");
  u8 pb[FILE_PATH_MAX_LEN];
  u8s ps = {};
  if (!JABCArgU8(ps, ctx, args[1], pb, sizeof(pb), exception)) JABC_UNDEF;
  u8cs path = {ps[0], ps[1]};
  b8 is_dir = (argc > 2 && JSValueToBoolean(ctx, args[2])) ? YES : NO;
  return JSValueMakeNumber(ctx, (double)IGNOSetDecide(&h->set, path, is_dir));
}

//  _igno_close(handle) -> undefined.  Unmaps the file; a 0 handle and a
//  second close are no-ops.
static JABC_FN(JABCignoClose) {
  if (argc < 1) JABC_THROW("igno: _igno_close needs a handle");
  jabc_igno* h = JABCignoHandle(ctx, args[0], exception);
  if (!h) return JSValueMakeUndefined(ctx);
  IGNOSetClose(&h->set);
  h->magic = 0;
  h->next = JABC_IGNO_SPARE;
  JABC_IGNO_SPARE = h;
  return JSValueMakeUndefined(ctx);
}

static inline void JABCIgnoInstall(JSObjectRef o) {
  JABC_API_FN(o, "_igno_open", JABCignoOpen);
  JABC_API_FN(o, "_igno_match", JABCignoMatch);
  JABC_API_FN(o, "_igno_close", JABCignoClose);
}

#endif
