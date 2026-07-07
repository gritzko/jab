//  C++ TUs include abc/PRO.h through this wrapper (§6 still applies: .cpp
//  files only, headers never).  Darwin clang compiles a C++ `thread_local`
//  into a hidden variable reached via a per-variable wrapper (_ZTW*) that
//  only the defining TU exports, so ABC_BASS could not be shared with abc's
//  C objects on macOS in either direction.  `__thread` (C-style TLS: plain
//  exported symbol, direct access, no wrapper) is exactly what abc's C TUs
//  emit for C23 thread_local, so the one ABC_BASS definition per binary can
//  live in a .cpp — declared __thread there too, next to the MAIN() setup.
#ifndef JABC_PRO_HPP
#define JABC_PRO_HPP
#define thread_local __thread
extern "C" {
#include "abc/PRO.h"
}
#undef thread_local
#endif
