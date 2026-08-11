#include "JABC.hpp"

//  JAB-035: the DEFAULT jsrc pack — a deflated ustar behind a 16-byte preamble,
//  embedded by jsrcpack.S (.incbin) when the build was given -DJAB_JSRC.  This
//  accessor is the whole native side: it publishes the bytes as the global
//  `jsrcpack` (a no-copy Uint8Array over the binary's own rodata), and the
//  require bootstrap (require.cpp) does the rest — cache probe, inflate, untar,
//  and the append of the extracted dir as the LAST jsrc stack entry (the floor).
//  No -DJAB_JSRC -> no .S, no symbol, no global, no floor: jab as it was.
#ifdef JABC_JSRC_PACK
extern "C" {
extern const unsigned char JABC_JSRC_PACK_HEAD[];
extern const unsigned char JABC_JSRC_PACK_TAIL[];
}
#endif

ok64 JABCJsrcPackInstall() {
#ifdef JABC_JSRC_PACK
  size_t len = (size_t)(JABC_JSRC_PACK_TAIL - JABC_JSRC_PACK_HEAD);
  if (len == 0) return OK;
  JSValueRef ex = NULL;
  //  No deallocator: the bytes are the executable image, not an allocation.
  JSValueRef ta = JSObjectMakeTypedArrayWithBytesNoCopy(
      JABC_CONTEXT, kJSTypedArrayTypeUint8Array,
      (void*)JABC_JSRC_PACK_HEAD, len, NULL, NULL, &ex);
  if (ex != NULL || ta == NULL) {
    JABCReport(ex);
    return OK;  //  no global -> no floor; the climb alone still resolves
  }
  JSStringRef k = JSStringCreateWithUTF8CString("jsrcpack");
  JSObjectSetProperty(JABC_CONTEXT, JABC_GLOBAL_OBJECT, k, ta,
                      kJSPropertyAttributeReadOnly |
                          kJSPropertyAttributeDontDelete,
                      NULL);
  JSStringRelease(k);
#endif
  return OK;
}
