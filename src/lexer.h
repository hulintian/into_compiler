#ifndef __LEXER_H__
#define __LEXER_H__

#include <string>


enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5
};

int gettok();

#endif
