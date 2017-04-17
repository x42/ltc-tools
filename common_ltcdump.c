#include "common_ltcdump.h"

void print_user_bits(FILE *outfile, const LTCFrame *f) {
  int user_bits  = f->user1;
  user_bits     += f->user2 * 16 <<  0;
  user_bits     += f->user3 * 16 <<  4;
  user_bits     += f->user4 * 16 <<  8;
  user_bits     += f->user5 * 16 << 12;
  user_bits     += f->user6 * 16 << 16;
  user_bits     += f->user7 * 16 << 20;
  user_bits     += f->user8 * 16 << 24;
  fprintf(outfile, "%08x" "%-3s", user_bits, "");
}
/* vi:set ts=8 sts=2 sw=2: */
