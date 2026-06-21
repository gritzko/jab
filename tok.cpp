#include "JABC.hpp"
#include "tok.hpp"

//  tok module install (JS-023): the _tok_parse_into leaf + the embedded TokStream
//  cursor.  Registered after JABCContInstall (it has no abc.* dependency, but
//  the lexer core lives in dog, already linked).  See tok.hpp for the leaf.
ok64 JABCTokInstall() {
  JABC_API_OBJECT(tok);
  JABC_API_FN(tok, "_tok_parse_into", JABCtokParseInto);
  JABCExecute(JABC_TOK_JS);
  return OK;
}
