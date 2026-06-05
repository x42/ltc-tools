#include "common_ltcdump.h"

void
print_user_bits (FILE* outfile, LTCFrame* f)
{
	unsigned long user_bits = ltc_frame_get_user_bits(f);
	fprintf (outfile, "%08lx" "%-3s", user_bits, "");
}
